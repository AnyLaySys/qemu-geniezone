#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

int gzvm_dev_ioctl(GZVMState *s, int type, void *arg)
{
    return ioctl(s->fd, type, arg);
}

int gzvm_vm_ioctl(int type, void *arg)
{
    GZVMState *s = GZVM_STATE(current_accel());
    return ioctl(s->vmfd, type, arg);
}

int gzvm_vcpu_ioctl(CPUState *cpu, int type, void *arg)
{
    return ioctl(GZVCPU(cpu)->fd, type, arg);
}
