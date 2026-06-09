#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

static GZVMState *gzvm_ioctl_state;

void gzvm_ioctl_set_state(GZVMState *s)
{
    gzvm_ioctl_state = s;
}

int gzvm_dev_ioctl(GZVMState *s, int type, void *arg)
{
    return ioctl(s->fd, type, arg);
}

int gzvm_vm_ioctl(int type, void *arg)
{
    assert(gzvm_ioctl_state != NULL);
    return ioctl(gzvm_ioctl_state->vmfd, type, arg);
}

int gzvm_vcpu_ioctl(CPUState *cpu, int type, void *arg)
{
    return ioctl(GZVCPU(cpu)->fd, type, arg);
}
