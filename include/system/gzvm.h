#ifndef QEMU_GZVM_H
#define QEMU_GZVM_H

#include "qemu/accel.h"
#include "qom/object.h"



#define TYPE_GZVM_ACCEL ACCEL_CLASS_NAME("gzvm")
typedef struct GZVMState GZVMState;
DECLARE_INSTANCE_CHECKER(GZVMState, GZVM_STATE,
                         TYPE_GZVM_ACCEL)

extern bool gzvm_allowed;

#define gzvm_enabled() (gzvm_allowed)

int gzvm_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size);
void gzvm_set_firmware(uint64_t start, uint64_t size);
void gzvm_set_gic_bases(uint64_t dist_base, uint64_t redist_base,
                        uint64_t redist_size);
void gzvm_set_ram_base(uint64_t base);

#endif
