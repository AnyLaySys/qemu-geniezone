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

static gzvm_slot *gzvm_find_overlap_slot(GZVMState *s, uint64_t start, uint64_t size)
{
    gzvm_slot *slot;
    int i;
    for (i = 0; i < s->nr_slots; ++i) {
        slot = &s->slots[i];
        if (slot->size && start < (slot->start + slot->size) &&
            (start + size) > slot->start) {
            return slot;
        }
    }
    return NULL;
}

gzvm_slot *gzvm_find_slot_by_addr(uint64_t addr)
{
    GZVMState *s = GZVM_STATE(current_accel());
    if (!s->nr_slots) {
        return NULL;
    }

    for (int i = 0; i < (int)s->nr_slots; ++i) {
        gzvm_slot *slot = &s->slots[i];
        if (!slot->size)
            continue;
        uint64_t start = slot->start;
        uint64_t size = slot->size;
        if (addr >= start && (addr - start) < size)
            return slot;
    }
    return NULL;
}

static gzvm_slot *gzvm_get_free_slot(GZVMState *s)
{
    if (s->free_slots) {
        guint slot_id = GPOINTER_TO_UINT(s->free_slots->data);
        s->free_slots = g_list_delete_link(s->free_slots, s->free_slots);
        return &s->slots[slot_id];
    }
    for (guint i = 0; i < s->nr_slots; i++) {
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
        exit(1);
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
        exit(1);
    }
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
            s->free_slots = g_list_prepend(s->free_slots,
                                           GUINT_TO_POINTER(slot->id));
        }
        gzvm_slots_unlock(s);
        return;
    }

    if (!memory_region_is_ram(area) && !memory_region_is_rom(area) &&
        !memory_region_is_romd(area)) {
        /* 
         * For MMIO regions, we don't map them as Guest RAM, but we still 
         * need to register them to ensure the GZVM hypervisor knows 
         * these ranges are valid for the VM.
         */
        gzvm_add_mem_slot(s, NULL, section->offset_within_address_space, 
                          int128_get64(section->size), 0);
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
        s->free_slots = g_list_prepend(s->free_slots,
                                       GUINT_TO_POINTER(slot->id));
    }

    if (s->protected_vm && (area->readonly || area->rom_device)) {
        flags = GZVM_USER_MEM_REGION_PROTECT_FW;
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
        exit(1);
    }

    ret = gzvm_dev_ioctl(s, GZVM_CREATE_VM, NULL);
    if (ret < 0) {
        error_report("GZVM_CREATE_VM failed: %s (errno=%d)",
                     strerror(errno), errno);
        exit(1);
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

    qemu_mutex_init(&s->slots_lock);
    s->free_slots = NULL;
    s->nr_slots = GZVM_MAX_MEM_SLOTS;
    for (int i = 0; i < s->nr_slots; ++i) {
        s->slots[i].start = 0;
        s->slots[i].size = 0;
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
            exit(1);
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
            exit(1);
        }
        s->gic_redist_base = 0x080A0000ULL;
    }

    return 0;
}
