#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "gzvm-internal.h"
#include "trace.h"

static void gzvm_assert_mutex_locked(QemuMutex *m)
{
    int ret = pthread_mutex_trylock(&m->lock);
    if (ret == 0) {
        pthread_mutex_unlock(&m->lock);
    }
    assert(ret == EBUSY);
}

/*
 * Binary-search sorted_ids[] for the first slot whose start >= addr.
 * Returns nr_active_slots if none found.
 */
static int gzvm_find_first_ge(GZVMState *s, uint64_t addr)
{
    int lo = 0, hi = (int)s->nr_active_slots - 1;
    int first_ge = (int)s->nr_active_slots;
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
    uint64_t end;
    int first_ge = gzvm_find_first_ge(s, start);

    if (!size || start > UINT64_MAX - size) {
        return NULL;
    }

    end = start + size;

    if (first_ge < (int)s->nr_active_slots) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[first_ge]];
        if (slot->start < end) {
            return slot;
        }
    }

    if (first_ge > 0) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[first_ge - 1]];
        if (slot->start <= start && slot->size > start - slot->start) {
            return slot;
        }
    }

    return NULL;
}

gzvm_slot *gzvm_find_slot_by_addr_locked(GZVMState *s, uint64_t addr)
{
    gzvm_assert_mutex_locked(&s->slots_lock);

    if (!s->nr_active_slots) {
        return NULL;
    }

    int lo = 0, hi = (int)s->nr_active_slots - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        gzvm_slot *slot = &s->slots[s->sorted_ids[mid]];
        if (addr < slot->start) {
            hi = mid - 1;
        } else if (addr - slot->start >= slot->size) {
            lo = mid + 1;
        } else {
            return slot;
        }
    }
    return NULL;
}

gzvm_slot *gzvm_find_slot_by_addr(uint64_t addr)
{
    AccelState *accel = current_accel();
    GZVMState *s;

    if (!accel) {
        return NULL;
    }
    s = GZVM_STATE(accel);

    gzvm_slots_lock(s);
    gzvm_slot *slot = gzvm_find_slot_by_addr_locked(s, addr);
    gzvm_slots_unlock(s);
    return slot;
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

static void gzvm_update_sorted_ids(GZVMState *s, int slot_id, bool add)
{
    int pos;

    if (add) {
        uint64_t gpa = s->slots[slot_id].start;
        pos = 0;
        while (pos < (int)s->nr_active_slots &&
               s->slots[s->sorted_ids[pos]].start < gpa) {
            pos++;
        }
        if (pos < (int)s->nr_active_slots) {
            memmove(&s->sorted_ids[pos + 1], &s->sorted_ids[pos],
                    (s->nr_active_slots - pos) * sizeof(gint));
        }
        s->sorted_ids[pos] = slot_id;
        s->nr_active_slots++;
    } else {
        for (pos = 0; pos < (int)s->nr_active_slots; pos++) {
            if (s->sorted_ids[pos] == slot_id) {
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

static int
gzvm_set_memory_region_locked(GZVMState *s, uint32_t slot, uint32_t flags,
                              uint64_t gpa, uint64_t size, void *hva)
{
    struct gzvm_userspace_memory_region gumr = {
        .slot = slot,
        .flags = flags,
        .guest_phys_addr = gpa,
        .memory_size = size,
        .userspace_addr = (__u64)(uintptr_t)hva,
    };
    gzvm_assert_mutex_locked(&s->slots_lock);
    return gzvm_vm_ioctl(GZVM_SET_USER_MEMORY_REGION, &gumr);
}

static void gzvm_mem_slot_deactivate_locked(GZVMState *s, gzvm_slot *slot)
{
    gzvm_assert_mutex_locked(&s->slots_lock);
    gzvm_update_sorted_ids(s, slot->id, false);
    slot->size = 0;
    slot->mem = NULL;
    slot->start = 0;
    slot->flags = 0;
    gzvm_signal_update_regions(s);
}

static int gzvm_remove_mem_slot_locked(GZVMState *s, gzvm_slot *slot)
{
    int ret;

    gzvm_assert_mutex_locked(&s->slots_lock);

    ret = gzvm_set_memory_region_locked(s, slot->id, 0,
                                        slot->start, 0, slot->mem);
    if (ret) {
        error_report("gzvm: remove memory slot %u failed: %s (errno=%d)",
                     slot->id, strerror(errno), errno);
        return ret;
    }
    trace_gzvm_del_mem_slot(slot->id, slot->start, slot->size);

    gzvm_mem_slot_deactivate_locked(s, slot);
    return 0;
}

static int gzvm_remove_overlap_slots_locked(GZVMState *s, uint64_t start,
                                             uint64_t size)
{
    gzvm_slot *slot;

    while ((slot = gzvm_find_overlap_slot(s, start, size))) {
        int ret = gzvm_remove_mem_slot_locked(s, slot);
        if (ret) {
            return ret;
        }
    }
    return 0;
}


static int gzvm_add_mem_slot(GZVMState *s, uint8_t *hva, uint64_t gpa,
                              uint64_t size,
                  uint32_t flags)
{
    gzvm_slot *slot;
    int ret;

    slot = gzvm_get_free_slot(s);
    if (!slot) {
        error_report("gzvm: No free memory slots available!");
        return -ENOSPC;
    }

    ret = gzvm_set_memory_region_locked(s, slot->id, flags,
                                        gpa, size, hva);
    trace_gzvm_add_mem_slot(slot->id, gpa, size, hva, flags);
    if (ret) {
        error_report("gzvm: GZVM_SET_USER_MEMORY_REGION failed: %s (errno=%d)",
                     strerror(errno), errno);
        return ret;
    }

    slot->size = size;
    slot->mem = hva;
    slot->start = gpa;
    slot->flags = flags;

    gzvm_update_sorted_ids(s, slot->id, true);
    gzvm_signal_update_regions(s);
    return 0;
}

static int gzvm_add_mem(GZVMState *s, MemoryRegionSection *section,
                        uint32_t flags)
{
    MemoryRegion *area = section->mr;
    uint64_t total_size = int128_get64(section->size);
    uint8_t *base_hva = memory_region_get_ram_ptr(area) +
                        section->offset_within_region;
    uint64_t base_gpa = section->offset_within_address_space;

    return gzvm_add_mem_slot(s, base_hva, base_gpa, total_size, flags);
}

static int
gzvm_add_mem_range(GZVMState *s, MemoryRegionSection *section,
                   uint64_t gpa, uint64_t size, uint32_t flags)
{
    MemoryRegion *area = section->mr;
    uint64_t section_start = section->offset_within_address_space;
    uint64_t section_end = section_start + int128_get64(section->size);
    uint64_t offset;
    uint8_t *hva;

    /* Sanity check: ensure gpa is within the section bounds */
    if (gpa < section_start || gpa + size > section_end) {
        error_report("gzvm: memory range [0x%" PRIx64 ", 0x%" PRIx64 
                     ") is out of section bounds [0x%" PRIx64 ", 0x%" PRIx64 ")",
                     gpa, gpa + size, section_start, section_end);
        return -EINVAL;
    }

    offset = gpa - section_start;
    hva = memory_region_get_ram_ptr(area) +
          section->offset_within_region + offset;

    return gzvm_add_mem_slot(s, hva, gpa, size, flags);
}

/*
 * Handle firmware region splitting for protected VMs.
 * Returns: 0 if no FW overlap (caller continues with normal add),
 *          1 if FW split was done (caller skips normal add),
 *         -1 on error (caller skips normal add).
 * Caller holds slots_lock.
 */
static int gzvm_set_phys_mem_fw_split(GZVMState *s,
                                       MemoryRegionSection *section,
                                       uint64_t section_start,
                                       uint64_t section_size,
                                       uint32_t flags)
{
    uint64_t page_size = qemu_real_host_page_size();
    uint64_t section_end = section_start + section_size;
    uint64_t fw_start = QEMU_ALIGN_DOWN(s->firmware_start, page_size);
    uint64_t fw_end = QEMU_ALIGN_UP(s->firmware_start + s->firmware_size,
                                    page_size);

    if (!(fw_start < section_end && fw_end > section_start)) {
        return 0;
    }

    uint64_t protect_start = MAX(fw_start, section_start);
    uint64_t protect_end = MIN(fw_end, section_end);

    if (protect_start > section_start) {
        if (gzvm_add_mem_range(s, section, section_start,
                               protect_start - section_start, flags)) {
            return -1;
        }
    }
    if (gzvm_add_mem_range(s, section, protect_start,
                           protect_end - protect_start,
                           GZVM_USER_MEM_REGION_PROTECT_FW)) {
        return -1;
    }
    if (protect_end < section_end) {
        if (gzvm_add_mem_range(s, section, protect_end,
                               section_end - protect_end, flags)) {
            return -1;
        }
    }
    return 1;
}

static void gzvm_set_phys_mem(GZVMState *s, MemoryRegionSection *section, bool add)
{
    MemoryRegion *area = section->mr;
    uint32_t flags = GZVM_USER_MEM_REGION_GUEST_MEM;
    uint64_t page_size = qemu_real_host_page_size();
    uint64_t section_start = section->offset_within_address_space;
    uint64_t section_size = int128_get64(section->size);

    if (!add) {
        gzvm_slots_lock(s);
        gzvm_remove_overlap_slots_locked(s, section_start, section_size);
        gzvm_slots_unlock(s);
        return;
    }

    if (!memory_region_is_ram(area) && !memory_region_is_rom(area) &&
        !memory_region_is_romd(area)) {
        return;
    }

    if (!QEMU_IS_ALIGNED(section_size, page_size) ||
        !QEMU_IS_ALIGNED(section_start, page_size)) {
        return;
    }

    if (section_start < s->ram_base &&
        !memory_region_is_rom(area) && !memory_region_is_romd(area)) {
        trace_gzvm_skip_region(section_start, section_size, s->ram_base);
        return;
    }

    gzvm_slots_lock(s);
    if (gzvm_remove_overlap_slots_locked(s, section_start, section_size)) {
        error_report("gzvm: failed to remove overlapping memory slots for "
                     "region [0x%" PRIx64 ", 0x%" PRIx64 ")",
                     section_start, section_start + section_size);
        gzvm_slots_unlock(s);
        return;
    }

    if (s->protected_vm && (area->readonly || area->rom_device)) {
        flags = GZVM_USER_MEM_REGION_PROTECT_FW;
    }

    if (s->protected_vm && s->firmware_size &&
        !area->readonly && !area->rom_device) {
        int fw_ret = gzvm_set_phys_mem_fw_split(s, section,
                                                 section_start, section_size,
                                                 flags);
        if (fw_ret < 0) {
            gzvm_remove_overlap_slots_locked(s, section_start, section_size);
            gzvm_slots_unlock(s);
            return;
        }
        if (fw_ret > 0) {
            gzvm_slots_unlock(s);
            return;
        }
    }

    if (gzvm_add_mem(s, section, flags)) {
        gzvm_slots_unlock(s);
        return;
    }

    gzvm_slots_unlock(s);
}

static void gzvm_region_add(MemoryListener *listener, MemoryRegionSection *section)
{
    AccelState *accel = current_accel();
    if (!accel) {
        return;
    }
    gzvm_set_phys_mem(GZVM_STATE(accel), section, true);
}

static void gzvm_region_del(MemoryListener *listener, MemoryRegionSection *section)
{
    AccelState *accel = current_accel();
    if (!accel) {
        return;
    }
    gzvm_set_phys_mem(GZVM_STATE(accel), section, false);
}

static MemoryListener gzvm_memory_listener = {
    .name = "gzvm",
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
    .region_add = gzvm_region_add,
    .region_del = gzvm_region_del,
};

static int gzvm_create_vgic_device(GZVMState *s,
                                    int dev_type, uint64_t dev_addr,
                                    uint64_t dev_reg_size,
                                    uint64_t *base_out, const char *name)
{
    struct gzvm_create_device dev = {
        .dev_type = dev_type,
        .dev_addr = dev_addr,
        .dev_reg_size = dev_reg_size,
    };
    int ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &dev);
    if (ret) {
        error_report("gzvm: create %s failed: %s (errno=%d)",
                     name, strerror(errno), errno);
        return ret;
    }
    *base_out = dev_addr;
    return 0;
}

static int gzvm_open_device(GZVMState *s)
{
    int ret;

    s->fd = qemu_open_old("/dev/gzvm", O_RDWR);
    if (s->fd == -1) {
        error_report("Could not access /dev/gzvm: %s", strerror(errno));
        return -1;
    }

    ret = gzvm_dev_ioctl(s, GZVM_CREATE_VM, NULL);
    if (ret < 0) {
        error_report("gzvm: GZVM_CREATE_VM failed: %s (errno=%d)",
                     strerror(errno), errno);
        close(s->fd);
        return -1;
    }
    s->vmfd = ret;
    trace_gzvm_create_vm(s->vmfd);
    return 0;
}

static void gzvm_probe_caps(GZVMState *s)
{
    {
        uint64_t cap = GZVM_CAP_ARM_VM_IPA_SIZE;
        int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION, &cap);
        if (r == 0) {
            trace_gzvm_ipa_size(cap);
            info_report("gzvm: IPA size: %d bits", (int)cap);
        } else {
            warn_report("gzvm: IPA size probe failed (r=%d), "
                        "assuming 40 bits", r);
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
                trace_gzvm_capability(cap_list[i].name, c);
                info_report("gzvm: cap %s = %" PRIu64, cap_list[i].name, c);
            }
        }
    }
}

static int gzvm_create_vgic_devices(GZVMState *s)
{
    int ret;

    ret = gzvm_create_vgic_device(s, GZVM_DEV_TYPE_ARM_VGIC_V3_DIST,
                                  0x08000000ULL, 0x10000,
                                  &s->gic_dist_base, "VGIC_DIST");
    if (ret) {
        close(s->vmfd);
        s->vmfd = -1;
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    trace_gzvm_vgic_dist_created(s->gic_dist_base);

    ret = gzvm_create_vgic_device(s, GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST,
                                  0x080A0000ULL, 0x20000ULL,
                                  &s->gic_redist_base, "VGIC_REDIST");
    if (ret) {
        close(s->vmfd);
        s->vmfd = -1;
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    trace_gzvm_vgic_redist_created(s->gic_redist_base);

    return 0;
}

int gzvm_create_vm(void)
{
    AccelState *accel = current_accel();
    GZVMState *s;

    if (!accel) {
        return -1;
    }
    s = GZVM_STATE(accel);

    if (gzvm_open_device(s)) {
        return -1;
    }

    gzvm_probe_caps(s);

    s->slots = g_new0(gzvm_slot, GZVM_MAX_MEM_SLOTS);
    s->sorted_ids = g_new0(gint, GZVM_MAX_MEM_SLOTS);
    qemu_mutex_init(&s->slots_lock);
    s->nr_active_slots = 0;
    for (int i = 0; i < GZVM_MAX_MEM_SLOTS; ++i) {
        s->slots[i].id = i;
    }

    gzvm_install_sigsegv_handler();
    trace_gzvm_sigsegv_handler_installed();
    memory_listener_register(&gzvm_memory_listener, &address_space_memory);
    memory_listener_register(&gzvm_ioeventfd_listener, &address_space_memory);

    return gzvm_create_vgic_devices(s);
}
