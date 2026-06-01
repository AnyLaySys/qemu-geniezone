#ifndef GZVM_INTERNAL_H
#define GZVM_INTERNAL_H

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/typedefs.h"
#include "hw/core/cpu.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"

/* gzvm-signal.c */
void gzvm_install_sigsegv_handler(void);

/* gzvm-ioctl.c */
int gzvm_dev_ioctl(GZVMState *s, int type, void *arg);

/* gzvm-vcpu.c */
int gzvm_detect_exit_reason(struct gzvm_vcpu_run *run);
void gzvm_cpu_kick_self(void);
void gzvm_init_cpu_signals(void);

/* gzvm-mmio.c */
int gzvm_handle_mmio_exit(CPUState *cpu, struct gzvm_vcpu_run *run);
int gzvm_handle_system_event(CPUState *cpu, struct gzvm_vcpu_run *run);
int gzvm_handle_fail_entry(CPUState *cpu, struct gzvm_vcpu_run *run);
int gzvm_handle_internal_error(CPUState *cpu, struct gzvm_vcpu_run *run);
int gzvm_handle_unknown_exit(CPUState *cpu, struct gzvm_vcpu_run *run);

/* gzvm-irq.c */
extern MemoryListener gzvm_ioeventfd_listener;

#endif
