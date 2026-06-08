#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "gzvm-internal.h"

#define gzvm_slots_lock(s)    qemu_mutex_lock(&s->slots_lock)
#define gzvm_slots_unlock(s)  qemu_mutex_unlock(&s->slots_lock)

/*
 * Binary-search sorted_ids[] for the first slot whose start >= addr.
 * Returns nr_active_slots if none found.
 */
static int gzvm_find_first_ge(GZVMState *s, uint64_t addr)
{
    int lo = 0, hi = s->nr_active_slots - 1;
    int first_ge = s->nr_active_slots;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        gzvm_slot *slot = &s->slots[s->sorted_ids[mid]];
        if (slot->start >= addr) {
            first_ge = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return first_ge;
}

static gzvm_slot *gzvm_find_overlap_slot(GZVMState *s, uint64_t start, uint64_t size)
{
    uint64_t end = start + size;
    int first_ge = gzvm_find_first_ge(s, start);

    if (first_ge < (int)s->nr_active_slots) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[first_ge]];
        if (slot->start < end) {
            return slot;
        }
    }

    if (first_ge > 0) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[first_ge - 1]];
        if (slot->start + slot->size > start) {
            return slot;
        }
    }

    return NULL;
}

gzvm_slot *gzvm_find_slot_by_addr(uint64_t addr)
{
    GZVMState *s = GZVM_STATE(current_accel());

    if (!s->nr_active_slots) {
        return NULL;
    }

    int lo = 0, hi = s->nr_active_slots - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        gzvm_slot *slot = &s->slots[s->sorted_ids[mid]];
        if (addr < slot->start) {
            hi = mid - 1;
        } else if (addr >= slot->start + slot->size) {
            lo = mid + 1;
        } else {
            return slot;
        }
    }
    return NULL;
}

static gzvm_slot *gzvm_get_free_slot(GZVMState *s)
{
    for (guint i = 0; i < GZVM_MAX_MEM_SLOTS; i++) {
        if (s->slots[i].size == 0) {
            return &s->slots[i];
        }
    }
    return NULL;
}

static void
gzvm_add_mem_slot(GZVMState *s, uint8_t *hva, uint64_t gpa, uint64_t size,
                  uint32_t flags)
{
    gzvm_slot *slot;
    struct gzvm_userspace_memory_region gumr;
    int ret;

    slot = gzvm_get_free_slot(s);
    if (!slot) {
        error_report("gzvm: No free memory slots available!");
        return;
    }

    slot->size = size;
    slot->mem = hva;
    slot->start = gpa;
    slot->flags = flags;

    gumr.slot = slot->id;
    gumr.flags = flags;
    gumr.guest_phys_addr = gpa;
    gumr.memory_size = size;
    gumr.userspace_addr = (__u64)(uintptr_t)hva;

    ret = gzvm_vm_ioctl(GZVM_SET_USER_MEMORY_REGION, &gumr);
    if (ret) {
        error_report("GZVM_SET_USER_MEMORY_REGION FAILED: %s (errno=%d)",
                     strerror(errno), errno);
        return;
    }

    /* Insert slot->id into sorted_ids, maintaining address order */
    int ins_pos = 0;
    while (ins_pos < (int)s->nr_active_slots &&
           s->slots[s->sorted_ids[ins_pos]].start < gpa) {
        ins_pos++;
    }
    if (ins_pos < (int)s->nr_active_slots) {
        memmove(&s->sorted_ids[ins_pos + 1], &s->sorted_ids[ins_pos],
                (s->nr_active_slots - ins_pos) * sizeof(gint));
    }
    s->sorted_ids[ins_pos] = slot->id;
    s->nr_active_slots++;
}

static void
gzvm_add_mem(GZVMState *s, MemoryRegionSection *section, uint32_t flags)
{
    MemoryRegion *area = section->mr;
    uint64_t total_size = int128_get64(section->size);
    uint8_t *base_hva = memory_region_get_ram_ptr(area) +
                        section->offset_within_region;
    uint64_t base_gpa = section->offset_within_address_space;

    gzvm_add_mem_slot(s, base_hva, base_gpa, total_size, flags);
}

static void
gzvm_add_mem_range(GZVMState *s, MemoryRegionSection *section,
                   uint64_t gpa, uint64_t size, uint32_t flags)
{
    MemoryRegion *area = section->mr;
    uint64_t offset = gpa - section->offset_within_address_space;
    uint8_t *hva = memory_region_get_ram_ptr(area) +
                   section->offset_within_region + offset;

    gzvm_add_mem_slot(s, hva, gpa, size, flags);
}

static void gzvm_set_phys_mem(GZVMState *s, MemoryRegionSection *section, bool add)
{
    MemoryRegion *area = section->mr;
    uint32_t flags = GZVM_USER_MEM_REGION_GUEST_MEM;
    uint64_t page_size = qemu_real_host_page_size();
    gzvm_slot *slot;

    if (!add) {
        gzvm_slots_lock(s);
        slot = gzvm_find_overlap_slot(s, section->offset_within_address_space,
                                       int128_get64(section->size));
        if (slot) {
            struct gzvm_userspace_memory_region gumr = {
                .slot = slot->id,
                .flags = 0,
                .guest_phys_addr = slot->start,
                .memory_size = 0,
                .userspace_addr = (__u64)(uintptr_t)slot->mem,
            };
            gzvm_vm_ioctl(GZVM_SET_USER_MEMORY_REGION, &gumr);
            slot->size = 0;
            /* Remove slot->id from sorted_ids */
            int pos;
            for (pos = 0; pos < (int)s->nr_active_slots; pos++) {
                if (s->sorted_ids[pos] == slot->id) {
                    break;
                }
            }
            if (pos < (int)s->nr_active_slots) {
                memmove(&s->sorted_ids[pos], &s->sorted_ids[pos + 1],
                        (s->nr_active_slots - pos - 1) * sizeof(gint));
                s->nr_active_slots--;
            }
        }
        gzvm_slots_unlock(s);
        return;
    }

    if (!memory_region_is_ram(area) && !memory_region_is_rom(area) &&
        !memory_region_is_romd(area)) {
        return;
    }

    if (!QEMU_IS_ALIGNED(int128_get64(section->size), page_size) ||
        !QEMU_IS_ALIGNED(section->offset_within_address_space, page_size)) {
        return;
    }

    if (section->offset_within_address_space < s->ram_base &&
        !memory_region_is_rom(area) && !memory_region_is_romd(area)) {
        return;
    }

    gzvm_slots_lock(s);
    slot = gzvm_find_overlap_slot(s, section->offset_within_address_space,
                                   int128_get64(section->size));
    if (slot) {
        struct gzvm_userspace_memory_region gumr = {
            .slot = slot->id,
            .flags = 0,
            .guest_phys_addr = slot->start,
            .memory_size = 0,
            .userspace_addr = (__u64)(uintptr_t)slot->mem,
        };
        gzvm_vm_ioctl(GZVM_SET_USER_MEMORY_REGION, &gumr);
        slot->size = 0;
        {
            int pos;
            for (pos = 0; pos < (int)s->nr_active_slots; pos++) {
                if (s->sorted_ids[pos] == slot->id) {
                    break;
                }
            }
            if (pos < (int)s->nr_active_slots) {
                memmove(&s->sorted_ids[pos], &s->sorted_ids[pos + 1],
                        (s->nr_active_slots - pos - 1) * sizeof(gint));
                s->nr_active_slots--;
            }
        }
    }

    if (s->protected_vm && (area->readonly || area->rom_device)) {
        flags = GZVM_USER_MEM_REGION_PROTECT_FW;
    }

    if (s->protected_vm && s->firmware_size &&
        !area->readonly && !area->rom_device) {
        uint64_t section_start = section->offset_within_address_space;
        uint64_t section_size = int128_get64(section->size);
        uint64_t section_end = section_start + section_size;
        uint64_t fw_start = QEMU_ALIGN_DOWN(s->firmware_start, page_size);
        uint64_t fw_end = QEMU_ALIGN_UP(s->firmware_start + s->firmware_size,
                                        page_size);

        if (fw_start < section_end && fw_end > section_start) {
            uint64_t protect_start = MAX(fw_start, section_start);
            uint64_t protect_end = MIN(fw_end, section_end);

            if (protect_start > section_start) {
                gzvm_add_mem_range(s, section, section_start,
                                   protect_start - section_start, flags);
            }
            gzvm_add_mem_range(s, section, protect_start,
                               protect_end - protect_start,
                               GZVM_USER_MEM_REGION_PROTECT_FW);
            if (protect_end < section_end) {
                gzvm_add_mem_range(s, section, protect_end,
                                   section_end - protect_end, flags);
            }
            gzvm_slots_unlock(s);
            return;
        }
    }

    gzvm_add_mem(s, section, flags);

    gzvm_slots_unlock(s);
}

static void gzvm_region_add(MemoryListener *listener, MemoryRegionSection *section)
{
    GZVMState *s = GZVM_STATE(current_accel());
    gzvm_set_phys_mem(s, section, true);
}

static void gzvm_region_del(MemoryListener *listener, MemoryRegionSection *section)
{
    GZVMState *s = GZVM_STATE(current_accel());
    gzvm_set_phys_mem(s, section, false);
}

static MemoryListener gzvm_memory_listener = {
    .name = "gzvm",
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
    .region_add = gzvm_region_add,
    .region_del = gzvm_region_del,
};

int gzvm_create_vm(void)
{
    GZVMState *s;
    int ret;

    s = GZVM_STATE(current_accel());

    s->fd = qemu_open_old("/dev/gzvm", O_RDWR);
    if (s->fd == -1) {
        error_report("Could not access /dev/gzvm: %s", strerror(errno));
        return -1;
    }

    ret = gzvm_dev_ioctl(s, GZVM_CREATE_VM, NULL);
    if (ret < 0) {
        error_report("GZVM_CREATE_VM failed: %s (errno=%d)",
                     strerror(errno), errno);
        return -1;
    }
    s->vmfd = ret;

    {
        uint64_t cap = GZVM_CAP_ARM_VM_IPA_SIZE;
        int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION, &cap);
        if (r == 0) {
            error_report("gzvm: IPA size: %d bits", (int)cap);
        } else {
            error_report("gzvm: IPA size probe failed (r=%d), assuming 40 bits",
                         r);
        }
    }

    {
        static const struct {
            uint64_t cap;
            const char *name;
        } cap_list[] = {
            { GZVM_CAP_ARM_PROTECTED_VM,     "PROTECTED_VM" },
            { GZVM_CAP_ENABLE_IDLE,          "ENABLE_IDLE" },
        };
        for (int i = 0; i < (int)ARRAY_SIZE(cap_list); i++) {
            uint64_t c = cap_list[i].cap;
            int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION, &c);
            if (r == 0) {
                error_report("gzvm: cap %s = %" PRIu64, cap_list[i].name, c);
            }
        }
    }

    s->slots = g_new0(gzvm_slot, GZVM_MAX_MEM_SLOTS);
    s->sorted_ids = g_new0(gint, GZVM_MAX_MEM_SLOTS);
    qemu_mutex_init(&s->slots_lock);
    s->nr_active_slots = 0;
    for (int i = 0; i < GZVM_MAX_MEM_SLOTS; ++i) {
        s->slots[i].id = i;
    }

    gzvm_install_sigsegv_handler();
    memory_listener_register(&gzvm_memory_listener, &address_space_memory);
    memory_listener_register(&gzvm_ioeventfd_listener, &address_space_memory);

    {
        struct gzvm_create_device dist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_DIST,
            .dev_addr = 0x08000000ULL,
            .dev_reg_size = 0x10000,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &dist_dev);
        if (ret) {
            error_report("gzvm: GZVM_CREATE_DEVICE VGIC_DIST failed: %s (errno=%d)",
                         strerror(errno), errno);
            return -1;
        }
        s->gic_dist_base = 0x08000000ULL;
    }

    {
        struct gzvm_create_device redist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST,
            .dev_addr = 0x080A0000ULL,
            .dev_reg_size = 0x20000ULL,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &redist_dev);
        if (ret) {
            error_report("gzvm: GZVM_CREATE_DEVICE VGIC_REDIST failed: %s (errno=%d)",
                         strerror(errno), errno);
            return -1;
        }
        s->gic_redist_base = 0x080A0000ULL;
    }

    return 0;
}
