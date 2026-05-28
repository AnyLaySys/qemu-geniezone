
static int gzvm_detect_exit_reason(struct gzvm_vcpu_run *run)
{
    if (run->mmio.size != 0) {
        return GZVM_EXIT_MMIO;
    }
    if (run->exception.exception != 0) {
        return GZVM_EXIT_EXCEPTION;
    }
    if (run->hypercall.args[0] != 0) {
        return GZVM_EXIT_HYPERCALL;
    }
    if (run->system_event.type != 0) {
        return GZVM_EXIT_SYSTEM_EVENT;
    }
    if (run->fail_entry.hardware_entry_failure_reason != 0) {
        return GZVM_EXIT_FAIL_ENTRY;
    }
    if (run->internal.suberror != 0 || run->internal.ndata != 0) {
        return GZVM_EXIT_INTERNAL_ERROR;
    }
    return 0;
}

static void gzvm_ipi_signal(int sig)
{
    if (current_cpu) {
        GZVCPU(current_cpu)->run->immediate_exit = 1;
    }
}

static void gzvm_cpu_kick_self(void)
{
    GZVCPU(current_cpu)->run->immediate_exit = 1;
}

static void gzvm_init_cpu_signals(void)
{
    struct sigaction sigact;

    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = gzvm_ipi_signal;
    sigaction(SIG_IPI, &sigact, NULL);
}

