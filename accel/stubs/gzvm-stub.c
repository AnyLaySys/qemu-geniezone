#include "qemu/osdep.h"
#include "system/gzvm.h"

bool gzvm_allowed;

int gzvm_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size)
{
    return -1;
}

void gzvm_set_ram_base(uint64_t base)
{
}
