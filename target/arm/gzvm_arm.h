#ifndef GZVM_ARM_H
#define GZVM_ARM_H

#include "cpu.h"

void gzvm_arm_set_cpu_features_from_host(ARMCPU *cpu);
void arm_cpu_gzvm_set_irq(void *arm_cpu, int irq, int level);

#endif
