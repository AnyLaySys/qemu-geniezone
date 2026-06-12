#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "system/runstate.h"
#include "gicv3_internal.h"
#include "migration/blocker.h"
#include "qom/object.h"
#include "target/arm/cpregs.h"
#include "qemu/event_notifier.h"

struct GZVMARMGICv3Class {
    ARMGICv3CommonClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define TYPE_GZVM_ARM_GICV3 "gzvm-arm-gicv3"
typedef struct GZVMARMGICv3Class GZVMARMGICv3Class;

DECLARE_OBJ_CHECKERS(GICv3State, GZVMARMGICv3Class,
                     GZVM_ARM_GICV3, TYPE_GZVM_ARM_GICV3)

static void gzvm_arm_gicv3_set_irq(void *opaque, int irq, int level)
{
    GICv3State *s = ARM_GICV3_COMMON(opaque);
    struct gzvm_irq_level irq_level;
    GICv3CPUState *cs;
    int irqtype;
    int cpu;

    if (irq < (int)(s->num_irq - GIC_INTERNAL)) {
        irqtype = GZVM_IRQ_TYPE_SPI;
        cpu = 0;
        irq += GIC_INTERNAL;
    } else {
        irqtype = GZVM_IRQ_TYPE_PPI;
        irq -= s->num_irq - GIC_INTERNAL;
        cpu = irq / GIC_INTERNAL;
        irq %= GIC_INTERNAL;
    }

    cs = &s->cpu[cpu];

    /*
     * Track the level in QEMU's GIC model so that gicv3_update()
     * can see pending interrupts when they become enabled later.
     */
    if (irqtype == GZVM_IRQ_TYPE_SPI) {
        gicv3_gicd_level_replace(s, irq, level);
    } else {
        if (level) {
            cs->level |= (1U << irq);
        } else {
            cs->level &= ~(1U << irq);
        }
    }

    /*
     * The kernel VGIC handles interrupt enable/disable itself.
     * Always inject via GZVM_IRQ_LINE — the kernel will mask
     * disabled interrupts.  Do NOT check QEMU's local GIC state
     * here, because guest GIC register accesses go to the kernel
     * VGIC directly and QEMU's state is stale.
     */

    irq_level.irq = (irqtype << GZVM_IRQ_TYPE_SHIFT) |
                    ((cpu & GZVM_IRQ_VCPU_MASK) << GZVM_IRQ_VCPU_SHIFT) |
                    (((cpu >> 8) & GZVM_IRQ_VCPU2_MASK) << GZVM_IRQ_VCPU2_SHIFT) |
                    (irq << GZVM_IRQ_NUM_SHIFT);
    irq_level.level = level;

    if (gzvm_vm_ioctl(GZVM_IRQ_LINE, &irq_level)) {
        warn_report("gzvm: GZVM_IRQ_LINE failed for irq=%d level=%d: %s",
                    irq, level, strerror(errno));
    }
}

static void gzvm_arm_gicv3_realize(DeviceState *dev, Error **errp)
{
    GICv3State *s = GZVM_ARM_GICV3(dev);
    GZVMARMGICv3Class *ggc = GZVM_ARM_GICV3_GET_CLASS(s);
    Error *local_err = NULL;

    ggc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (s->revision != 3) {
        error_setg(errp, "unsupported GIC revision %d",
                   s->revision);
        return;
    }

    /*
     * Pass NULL ops so that DIST/REDIST MMIO regions are mapped as
     * pass-through — guest accesses go directly to the kernel VGIC.
     * The kernel intercepted GIC register writes already; having QEMU
     * also trap them causes stale GIC state and breaks interrupt delivery.
     */
    gicv3_init_irqs_and_mmio(s, gzvm_arm_gicv3_set_irq, NULL);
}

static void gzvm_arm_gicv3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    GZVMARMGICv3Class *ggc = GZVM_ARM_GICV3_CLASS(klass);

    device_class_set_parent_realize(dc, gzvm_arm_gicv3_realize,
                                    &ggc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, NULL, NULL,
                                       &ggc->parent_phases);
}

static const TypeInfo gzvm_arm_gicv3_info = {
    .name = TYPE_GZVM_ARM_GICV3,
    .parent = TYPE_ARM_GICV3_COMMON,
    .instance_size = sizeof(GICv3State),
    .class_init = gzvm_arm_gicv3_class_init,
    .class_size = sizeof(GZVMARMGICv3Class),
};

static void gzvm_arm_gicv3_register_types(void)
{
    type_register_static(&gzvm_arm_gicv3_info);
}

type_init(gzvm_arm_gicv3_register_types)
