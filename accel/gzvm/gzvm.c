#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "hw/core/cpu.h"
#include "system/cpus.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "exec/cpu-common.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "qapi/error.h"
#include "qemu/event_notifier.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "qemu/guest-random.h"

#define gz0 "\033[32mgzvm:\033[0m"

#include "gzvm-signal.c"
#include "gzvm-ioctl.c"
#include "gzvm-mem.c"
#include "gzvm-irq.c"
#include "gzvm-vm-start.c"
#include "gzvm-mmio.c"
#include "gzvm-vcpu.c"

static int gzvm_init_vcpu(CPUState *cpu)
{
    struct GZVCPUState *vcpu = g_new0(struct GZVCPUState, 1);
    int ret;

    ret = gzvm_vm_ioctl(GZVM_CREATE_VCPU, (void *)(uintptr_t)cpu->cpu_index);
    if (ret < 0) {
        g_free(vcpu);
        error_report("GZVM_CREATE_VCPU failed: %s (errno=%d)",
                     strerror(errno), errno);
        return ret;
    }

    vcpu->fd = ret;
    vcpu->run = g_new0(struct gzvm_vcpu_run, 1);
    cpu->accel = (AccelCPUState *)vcpu;
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
        error_report("GZVM_RUN failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    if (!run->exit_reason) {
        run->exit_reason = gzvm_detect_exit_reason(run);
    }

    switch (run->exit_reason) {
    case GZVM_EXIT_MMIO:
        return gzvm_handle_mmio_exit(cpu, run);
    case GZVM_EXIT_SYSTEM_EVENT:
        fprintf(stderr, "gzvm SYSTEM_EVENT type=%d\n", run->system_event.type);
        return gzvm_handle_system_event(cpu, run);
    case GZVM_EXIT_FAIL_ENTRY:
        return gzvm_handle_fail_entry(cpu, run);
    case GZVM_EXIT_INTERNAL_ERROR:
        return gzvm_handle_internal_error(cpu, run);
    case GZVM_EXIT_IDLE:
        fprintf(stderr, "gzvm IDLE\n");
        return EXCP_INTERRUPT;
    case GZVM_EXIT_IRQ:
        fprintf(stderr, "gzvm IRQ\n");
        return EXCP_INTERRUPT;
    case GZVM_EXIT_HYPERCALL:
        fprintf(stderr, "gzvm HYPERCALL\n");
        return 0;
    case GZVM_EXIT_GZ:
        fprintf(stderr, "gzvm GZ\n");
        return EXCP_INTERRUPT;
    case GZVM_EXIT_IPI:
        fprintf(stderr, "gzvm IPI\n");
        return EXCP_INTERRUPT;
    case 0:
        return 0;
    default:
        return gzvm_handle_unknown_exit(cpu, run);
    }
}

static void do_gzvm_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    if (gzvm_arch_put_registers(cpu, 0)) {
        cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }
}

void gzvm_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_gzvm_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void *gzvm_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int ret;

    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    gzvm_init_cpu_signals();

    ret = gzvm_init_vcpu(cpu);
    if (ret) {
        cpu_thread_signal_created(cpu);
        cpu_thread_signal_destroyed(cpu);
        bql_unlock();
        rcu_unregister_thread();
        return NULL;
    }

    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_process_cpu_events(cpu);

        if (cpu_can_run(cpu)) {
            ret = gzvm_cpu_exec(cpu);
            if (ret < 0) {
                cpu_dump_state(cpu, stderr, CPU_DUMP_CODE);
                vm_stop(RUN_STATE_INTERNAL_ERROR);
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    close(GZVCPU(cpu)->fd);
    g_free(GZVCPU(cpu)->run);
    g_free(cpu->accel);
    cpu->accel = NULL;
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}
