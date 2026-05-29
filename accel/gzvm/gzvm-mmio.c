static int gzvm_handle_mmio_exit(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    address_space_rw(&address_space_memory, run->mmio.phys_addr,
                     MEMTXATTRS_UNSPECIFIED,
                     run->mmio.data, run->mmio.size, run->mmio.is_write);
    return 0;
}

static int gzvm_handle_system_event(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    switch (run->system_event.type) {
    case GZVM_SYSTEM_EVENT_SHUTDOWN:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return EXCP_INTERRUPT;
    case GZVM_SYSTEM_EVENT_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return EXCP_INTERRUPT;
    case GZVM_SYSTEM_EVENT_CRASH:
        qemu_system_guest_panicked(cpu_get_crash_info(cpu));
        return 0;
    case GZVM_SYSTEM_EVENT_WAKEUP:
        /*
         * Hypervisor waking a previously powered-down VCPU (PSCI CPU_ON).
         * Clear halted so the VCPU thread re-enters GZVM_RUN.
         */
        cpu->halted = 0;
        return EXCP_INTERRUPT;
    default:
        return 0;
    }
}

static int gzvm_handle_fail_entry(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm    │CPU#%d FAIL_ENTRY reason=0x%" PRIx64 " cpu=%u",
                 cpu->cpu_index,
                 (uint64_t)run->fail_entry.hardware_entry_failure_reason,
                 run->fail_entry.cpu);
    return -1;
}

static int gzvm_handle_internal_error(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm    │CPU#%d INTERNAL_ERROR suberror=%u ndata=%u",
                 cpu->cpu_index, run->internal.suberror, run->internal.ndata);
    for (int _i = 0; _i < run->internal.ndata && _i < 16; _i++) {
        error_report("gzvm    │  data[%d] = 0x%llx", _i,
                     (unsigned long long)run->internal.data[_i]);
    }
    return -1;
}

static int gzvm_handle_unknown_exit(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm    │CPU#%d unknown exit_reason=0x%x",
                 cpu->cpu_index, run->exit_reason);
    return 0;
}
