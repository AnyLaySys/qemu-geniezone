#include <unistd.h>

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
    int i;
    gzvm_slot *slot = NULL;
    gzvm_slots_lock(s);
    for (i = 0; i < s->nr_slots; ++i) {
        slot = &s->slots[i];
        if (slot->size && (addr >= slot->start && addr < slot->start + slot->size))
            break;
    }
    gzvm_slots_unlock(s);
    return slot;
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
        error_report("No free slots to add memory!");
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

    error_report("gzvm    │ADD_MEM_SLOT slot=%d gpa=0x%llx size=0x%llx hva=%p flags=0x%x",
                 slot->id, (unsigned long long)gpa, (unsigned long long)size,
                 hva, flags);

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

    if (!memory_region_is_ram(area) &&
        !memory_region_is_rom(area) &&
        !memory_region_is_romd(area)) {
        return;
    }

    if (!QEMU_IS_ALIGNED(int128_get64(section->size), page_size) ||
        !QEMU_IS_ALIGNED(section->offset_within_address_space, page_size)) {
        return;
    }

    /* Skip secure/MMIO regions below RAM; keep firmware ROM/ROMD regions */
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

    /*
     * Only mark readonly/rom regions as PROTECT_FW when running as a
     * protected VM.  For non-protected VMs use GUEST_MEM for everything,
     * matching crosvm's GenieZone backend which never passes PROTECT_FW
     * to the hypervisor unless runs_firmware() returns true (pVM path).
     * The hypervisor may reject PROTECT_FW regions in non-pVM mode.
     */
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

extern MemoryListener gzvm_ioeventfd_listener;

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

    ret = gzvm_ioctl(GZVM_CREATE_VM, NULL);
    if (ret < 0) {
        error_report("GZVM_CREATE_VM failed: %s (errno=%d)",
                     strerror(errno), errno);
        exit(1);
    }
    s->vmfd = ret;

    {
        /*
         * GZVM_CHECK_EXTENSION passes the extension number as the ioctl arg
         * and returns the feature value (>0) on success, 0 if unsupported.
         * Do NOT pass &cap — the kernel reads the arg directly as an unsigned
         * long, not as a pointer to a value.
         */
        int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION,
                              (void *)(uintptr_t)GZVM_CAP_ARM_VM_IPA_SIZE);
        if (r > 0) {
            error_report("gzvm    │IPA size: %d bits", r);
        } else {
            error_report("gzvm    │IPA size probe failed (r=%d), assuming 40 bits",
                         r);
        }
    }

    /* Probe key capabilities for diagnostics */
    {
        static const struct {
            uint64_t cap;
            const char *name;
        } cap_list[] = {
            { GZVM_CAP_ARM_PROTECTED_VM,     "PROTECTED_VM" },
            { GZVM_CAP_ENABLE_IDLE,          "ENABLE_IDLE" },
        };
        for (int i = 0; i < (int)ARRAY_SIZE(cap_list); i++) {
            int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION,
                                  (void *)(uintptr_t)cap_list[i].cap);
            if (r > 0) {
                error_report("gzvm    │cap %s = %d", cap_list[i].name, r);
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

    /*
     * Create VGICv3 DIST before any VCPUs, so the kernel driver's
     * gzvm_vgic_create() sets vgic.in_kernel = true.
     * Kernel driver hardcodes DIST base to 0x08000000 internally,
     * so we don't need the final GIC base address yet.
     */
    {
        struct gzvm_create_device dist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_DIST,
            .dev_addr = 0x08000000ULL,
            .dev_reg_size = 0x10000,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &dist_dev);
        if (ret) {
            error_report("gzvm    │GZVM_CREATE_DEVICE VGIC_DIST failed: %s (errno=%d)",
                         strerror(errno), errno);
            exit(1);
        }
        error_report("gzvm    │VGICv3 DIST created early at 0x8000000");
        s->gic_dist_base = 0x08000000ULL;
    }

    /*
     * REDIST must also be created before VCPUs, matching crosvm's order:
     * create both DIST and REDIST first, then VCPUs.
     * Kernel driver ignores dev_addr/dev_reg_size for REDIST (hardcoded
     * internally to base=0x080A0000, count=0/unlimited), but we pass
     * reasonable values for forward compatibility.
     */
    {
        struct gzvm_create_device redist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST,
            .dev_addr = 0x080A0000ULL,
            .dev_reg_size = 0x20000ULL,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &redist_dev);
        if (ret) {
            error_report("gzvm    │GZVM_CREATE_DEVICE VGIC_REDIST failed: %s (errno=%d)",
                         strerror(errno), errno);
            exit(1);
        }
        error_report("gzvm    │VGICv3 REDIST created early at 0x80A0000");
        s->gic_redist_base = 0x080A0000ULL;
    }

    error_report("gzvm    │gzvm_create_vm: VM fd=%d memory listener registered", s->vmfd);
    return 0;
}
