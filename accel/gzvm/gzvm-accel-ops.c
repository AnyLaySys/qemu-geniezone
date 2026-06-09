#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "accel/accel-cpu-ops.h"
#include "accel/accel-ops.h"
#include "system/cpus.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"
#include "qapi/error.h"
#include "migration/blocker.h"

bool gzvm_allowed;

static Error *gzvm_migration_blocker;

static int gzvm_init(AccelState *as, MachineState *ms)
{
    int ret;
    Error *local_err = NULL;

    gzvm_ioctl_set_state(GZVM_STATE(as));

    ret = gzvm_create_vm();
    if (ret) {
        return ret;
    }

    error_setg(&gzvm_migration_blocker,
               "gzvm: migration not supported (no dirty page tracking or GET_ONE_REG in kernel API)");
    ret = migrate_add_blocker(&gzvm_migration_blocker, &local_err);
    if (ret < 0) {
        error_report_err(local_err);
        error_free(gzvm_migration_blocker);
        gzvm_migration_blocker = NULL;
        return ret;
    }

    return 0;
}

static bool gzvm_get_protected(Object *obj, Error **errp)
{
    GZVMState *s = GZVM_STATE(obj);
    return s->protected_vm;
}

static void gzvm_set_protected(Object *obj, bool value, Error **errp)
{
    GZVMState *s = GZVM_STATE(obj);
    s->protected_vm = value;
}

static void gzvm_accel_instance_finalize(Object *obj)
{
    GZVMState *s = GZVM_STATE(obj);
    if (gzvm_migration_blocker) {
        migrate_del_blocker(&gzvm_migration_blocker);
    }
    g_free(s->slots);
    g_free(s->sorted_ids);
}

static void gzvm_accel_instance_init(Object *obj)
{
    GZVMState *s = GZVM_STATE(obj);
    s->fd = -1;
    s->vmfd = -1;
    s->protected_vm = false;
    s->slots = NULL;
    s->sorted_ids = NULL;
    object_property_add_bool(obj, "protected",
                             gzvm_get_protected,
                             gzvm_set_protected);
}

static void gzvm_setup_post(AccelState *accel)
{
    gzvm_start_vm();
}

static void gzvm_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "GZVM";
    ac->init_machine = gzvm_init;
    ac->allowed = &gzvm_allowed;
    ac->setup_post = gzvm_setup_post;
}

static const TypeInfo gzvm_accel_type = {
    .name = TYPE_GZVM_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_init = gzvm_accel_instance_init,
    .instance_finalize = gzvm_accel_instance_finalize,
    .class_init = gzvm_accel_class_init,
    .instance_size = sizeof(GZVMState),
};

static void gzvm_type_init(void)
{
    type_register_static(&gzvm_accel_type);
}

type_init(gzvm_type_init);

static void gzvm_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/GZVM",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, gzvm_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void gzvm_kick_vcpu_thread(CPUState *cpu)
{
    cpus_kick_thread(cpu);
}

static bool gzvm_vcpu_thread_is_idle(CPUState *cpu)
{
    return false;
}

static void gzvm_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);
    ops->create_vcpu_thread = gzvm_start_vcpu_thread;
    ops->kick_vcpu_thread = gzvm_kick_vcpu_thread;
    ops->cpu_thread_is_idle = gzvm_vcpu_thread_is_idle;
    ops->synchronize_post_reset = gzvm_cpu_synchronize_post_reset;
    ops->synchronize_post_init = gzvm_cpu_synchronize_post_reset;
    ops->handle_interrupt = generic_handle_interrupt;
}

static const TypeInfo gzvm_accel_ops_type = {
    .name = ACCEL_OPS_NAME("gzvm"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = gzvm_accel_ops_class_init,
    .abstract = true,
};

static void gzvm_accel_ops_register_types(void)
{
    type_register_static(&gzvm_accel_ops_type);
}

type_init(gzvm_accel_ops_register_types);
