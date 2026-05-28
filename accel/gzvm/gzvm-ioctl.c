static int gzvm_ioctl(int type, ...)
{
    void *arg;
    va_list ap;
    GZVMState *s = GZVM_STATE(current_accel());
    assert(s->fd >= 0);
    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);
    return ioctl(s->fd, type, arg);
}

int gzvm_vm_ioctl(int type, ...)
{
    void *arg;
    va_list ap;
    GZVMState *s = GZVM_STATE(current_accel());
    assert(s->vmfd >= 0);
    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);
    return ioctl(s->vmfd, type, arg);
}

int gzvm_vcpu_ioctl(CPUState *cpu, int type, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);
    return ioctl(GZVCPU(cpu)->fd, type, arg);
}
