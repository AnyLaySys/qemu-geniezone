#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "hw/core/cpu.h"
#include "system/cpus.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "qemu/guest-random.h"
#include "gzvm-internal.h"
#include "trace.h"

static int gzvm_init_vcpu(CPUState *cpu)
{
    struct GZVCPUState *vcpu = g_new0(struct GZVCPUState, 1);
    int ret;

    ret = gzvm_vm_ioctl(GZVM_CREATE_VCPU, (void *)(uintptr_t)cpu->cpu_index);
    if (ret < 0) {
        g_free(vcpu);
        error_report("gzvm: GZVM_CREATE_VCPU failed: %s (errno=%d)",
                     strerror(errno), errno);
        return ret;
    }

    vcpu->fd = ret;
    vcpu->run = g_new0(struct gzvm_vcpu_run, 1);
    qatomic_set(&cpu->accel, (AccelCPUState *)vcpu);
    return 0;
}

static int gzvm_cpu_exec(CPUState *cpu)
{
    struct gzvm_vcpu_run *run = GZVCPU(cpu)->run;
    int ret;

    run->immediate_exit = 0;
    bql_unlock();
    if (qatomic_load_acquire(&cpu->exit_request)) {
        gzvm_cpu_kick_self();
    }
    ret = gzvm_vcpu_ioctl(cpu, GZVM_RUN, run);
    bql_lock();
    if (ret < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return EXCP_INTERRUPT;
        }
        error_report("gzvm: GZVM_RUN failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    switch (run->exit_reason) {
    case GZVM_EXIT_MMIO: {
        return gzvm_handle_mmio_exit(cpu, run);
    }
    case GZVM_EXIT_SYSTEM_EVENT:
        return gzvm_handle_system_event(cpu, run);
    case GZVM_EXIT_FAIL_ENTRY:
        return gzvm_handle_fail_entry(cpu, run);
    case GZVM_EXIT_INTERNAL_ERROR:
        return gzvm_handle_internal_error(cpu, run);
    case GZVM_EXIT_IDLE:
        return EXCP_INTERRUPT;
    case GZVM_EXIT_IRQ:
        return EXCP_INTERRUPT;
    case GZVM_EXIT_HYPERCALL:
        /*
         * GenieZone at EL2 handles PSCI CPU_ON/OFF/SUSPEND etc.
         * in-kernel.  Hypercalls reaching QEMU are unrecognised.
         * The return value in x0 is left as the kernel set it.
         */
        warn_report("gzvm: VCPU%u unhandled hypercall fn=0x%" PRIx64,
                    cpu->cpu_index, (uint64_t)run->hypercall.args[0]);
        return EXCP_INTERRUPT;
    case GZVM_EXIT_GZ:
        return EXCP_INTERRUPT;
    case GZVM_EXIT_IPI:
        return EXCP_INTERRUPT;
    case GZVM_EXIT_DEBUG:
        return EXCP_DEBUG;
    case GZVM_EXIT_SHUTDOWN:
        if (cpu->cpu_index == 0) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            return EXCP_INTERRUPT;
        }
        return EXCP_INTERRUPT;
    case GZVM_EXIT_EXCEPTION:
        error_report("gzvm: VCPU%u exception: type=%u error_code=0x%x "
                     "fault_gpa=0x%" PRIx64,
                     cpu->cpu_index, run->exception.exception,
                     run->exception.error_code,
                     (uint64_t)run->exception.fault_gpa);
        return -1;
    case 0:
        return 0;
    default:
        return gzvm_handle_unknown_exit(cpu, run);
    }
}

static void do_gzvm_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    int ret = gzvm_arch_put_registers(cpu, 0);
    if (ret) {
        warn_report("gzvm: VCPU%u put_registers failed with %d",
                    cpu->cpu_index, ret);
    }
}

void gzvm_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_gzvm_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

static bool gzvm_cpu_thread_init(CPUState *cpu)
{
    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    gzvm_init_cpu_signals();
    gzvm_init_vcpu_sigsegv();

    if (gzvm_init_vcpu(cpu)) {
        cpu_thread_signal_destroyed(cpu);
        bql_unlock();
        rcu_unregister_thread();
        return false;
    }

    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);
    return true;
}

static void gzvm_cpu_thread_cleanup(CPUState *cpu)
{
    struct GZVCPUState *vcpu = GZVCPU(cpu);

    close(vcpu->fd);
    qatomic_set(&cpu->accel, NULL);
    g_free(vcpu->run);
    g_free(vcpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
}

void *gzvm_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int ret;

    if (!gzvm_cpu_thread_init(cpu)) {
        return NULL;
    }

    do {
        qemu_process_cpu_events(cpu);

        if (cpu_can_run(cpu)) {
            ret = gzvm_cpu_exec(cpu);
            if (ret == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            } else if (ret < 0) {
                error_report("gzvm: VCPU%u run error ret=%d", cpu->cpu_index, ret);
                vm_stop(RUN_STATE_INTERNAL_ERROR);
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    gzvm_cpu_thread_cleanup(cpu);
    return NULL;
}
