#include "qemu/osdep.h"
#include "cpu.h"
#include "gzvm_arm.h"

void arm_cpu_gzvm_set_irq(void *arm_cpu, int irq, int level)
{
    g_assert_not_reached();
}
