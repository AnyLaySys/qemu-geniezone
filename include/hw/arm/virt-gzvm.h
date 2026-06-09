#ifndef VIRT_GZVM_H
#define VIRT_GZVM_H

#include "hw/arm/virt.h"

void virt_gzvm_init(VirtMachineState *vms);
void virt_gzvm_post_gic(VirtMachineState *vms);
void virt_gzvm_post_dtb(VirtMachineState *vms, hwaddr dtb_start, int dtb_size,
                        AddressSpace *as);
void virt_gzvm_disable_highmem(VirtMachineState *vms);
void virt_gzvm_create_virtio_gpu(VirtMachineState *vms);
void virt_gzvm_set_bootinfo(VirtMachineState *vms, bool firmware_loaded);

#endif
