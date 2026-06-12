#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"
#include "hw/arm/virt-gzvm.h"
#include "hw/core/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/pci/pci.h"
#include "system/gzvm.h"
#include "system/system.h"

void virt_gzvm_init(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    gzvm_set_ram_base(vms->memmap[VIRT_MEM].base);
}

void virt_gzvm_post_gic(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    gzvm_set_gic_bases(vms->memmap[VIRT_GIC_DIST].base,
                       vms->memmap[VIRT_GIC_REDIST].base,
                       vms->memmap[VIRT_GIC_REDIST].size);
}

void virt_gzvm_post_dtb(VirtMachineState *vms, hwaddr dtb_start, int dtb_size,
                        AddressSpace *as)
{
    void *dtb_data;

    if (!gzvm_enabled()) {
        return;
    }

    gzvm_arm_set_dtb(dtb_start, dtb_size);
    dtb_data = rom_ptr_for_as(as, dtb_start, dtb_size);
    if (dtb_data) {
        fw_cfg_add_file(vms->fw_cfg, "etc/fdt",
                        g_memdup2(dtb_data, dtb_size), dtb_size);
    }
}

void virt_gzvm_disable_highmem(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    /*
     * GZVM supports highmem PCI ECAM and MMIO — the guest accesses
     * these via MMIO traps handled by QEMU.  No kernel-side memory
     * slot registration is needed.  Let the user's choice stand.
     */
}

void virt_gzvm_create_virtio_gpu(VirtMachineState *vms)
{
    if (!gzvm_enabled() || !vms->bus || !MACHINE(vms)->enable_graphics ||
        vga_interface_created) {
        return;
    }

    pci_create_simple(vms->bus, -1, "virtio-gpu-pci");
}

void virt_gzvm_set_bootinfo(VirtMachineState *vms, bool firmware_loaded)
{
    if (!gzvm_enabled() || !firmware_loaded) {
        return;
    }

    vms->bootinfo.entry = vms->memmap[VIRT_MEM].base;
    vms->bootinfo.dtb_start = vms->memmap[VIRT_MEM].base + 4 * MiB;
}
