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
#include "trace.h"
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

    /*
     * QEMU GICv3 GPIO array layout (from gicv3_init_irqs_and_mmio):
     *   [0..N-1]              SPIs  (N = num_irq - GIC_INTERNAL)
     *   [N..N+31]             PPIs for CPU 0
     *   [N+32..N+63]          PPIs for CPU 1
     *   ...
     *
     * GIC interrupt numbering in ARM GICv3:
     *   SGIs  = 0..15   (private, not routed here)
     *   PPIs  = 16..31  (private per-CPU)
     *   SPIs  = 32..287 (shared, extended to 1019 in GICv3)
     *
     * For SPI GPIO lines (irq < N): convert the GPIO index to the
     * actual GIC SPI number by adding GIC_INTERNAL (32).
     *
     * For PPI GPIO lines (irq >= N): decode the CPU index and
     * the per-CPU PPI number (0..31).  The kernel GZVM_IRQ_TYPE_PPI
     * interface handles SGIs (0..15) as part of the PPI range;
     * only PPIs (16..31) are actually wired as GPIO inputs.
     */
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

/*
 * Distributor MMIO ops — never called because the kernel VGIC traps
 * all distributor accesses via stage-2 page tables.
 */
static MemTxResult gzvm_gicv3_dist_read(void *opaque, hwaddr offset,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    trace_gzvm_gicv3_dist_read(offset, size);
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult gzvm_gicv3_dist_write(void *opaque, hwaddr offset,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    trace_gzvm_gicv3_dist_write(offset, data, size);
    return MEMTX_OK;
}

static const MemoryRegionOps gzvm_gicv3_dist_ops = {
    .read_with_attrs = gzvm_gicv3_dist_read,
    .write_with_attrs = gzvm_gicv3_dist_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/*
 * GZVM kernel VGIC traps the redistributor RD_PAGE (GICR_CTLR, GICR_WAKER,
 * GICR_TYPER at offset 0x0000–0xFFFF) but NOT the SGI_PAGE (ISENABLER0,
 * ICENABLER0, IPRIORITYR etc. at offset 0x10000–0x1FFFF).  Accesses to
 * the SGI_PAGE leak to QEMU as MMIO exits.
 *
 * Use the standard GICv3 redistributor read/write handlers so that
 * SGI/PPI register accesses are correctly emulated.  RD_PAGE accesses
 * are still trapped by the kernel and never reach QEMU.
 *
 * gicv3_cpuif_update() called from gicr_writel is harmless: QEMU's
 * gicd_ctlr stays at reset (no EN_GRP bits set), so gicr_int_pending()
 * always returns 0 and no interrupt is asserted through QEMU's GIC model.
 * All interrupt injection remains under kernel VGIC control via
 * GZVM_IRQ_LINE.
 */
static const MemoryRegionOps gzvm_gicv3_redist_ops = {
    .read_with_attrs = gicv3_redist_read,
    .write_with_attrs = gicv3_redist_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps gzvm_gicv3_ops[2] = {
    [0] = gzvm_gicv3_dist_ops,
    [1] = gzvm_gicv3_redist_ops,
};

static void gzvm_arm_gicv3_realize(DeviceState *dev, Error **errp)
{
    GICv3State *s = GZVM_ARM_GICV3(dev);
    GZVMARMGICv3Class *ggc = GZVM_ARM_GICV3_GET_CLASS(s);
    Error *local_err = NULL;

    if (s->revision != 3) {
        error_setg(errp, "unsupported GIC revision %d",
                   s->revision);
        return;
    }

    ggc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    gicv3_init_irqs_and_mmio(s, gzvm_arm_gicv3_set_irq, gzvm_gicv3_ops);
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
