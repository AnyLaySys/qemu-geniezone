#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "cpu.h"
#include "internals.h"
#include "gzvm_arm.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"

#define GZVM_CORE_REG(offset)  (GZVM_REG_ARM64 | GZVM_REG_SIZE_U64 | \
                                GZVM_REG_ARM_CORE | ((offset) / 4))
#define GZVM_CORE_REG32(offset) (GZVM_REG_ARM64 | GZVM_REG_SIZE_U32 | \
                                 GZVM_REG_ARM_CORE | ((offset) / 4))
#define GZVM_CORE_REG128(offset) (GZVM_REG_ARM64 | GZVM_REG_SIZE_U128 | \
                                  GZVM_REG_ARM_CORE | ((offset) / 4))

#define GZVM_REGS_X(i)      ((i) * 8)
#define GZVM_REGS_SP        (31 * 8)
#define GZVM_REGS_PC        (32 * 8)
#define GZVM_REGS_PSTATE    (33 * 8)
#define GZVM_REGS_SP_EL1    (34 * 8)
#define GZVM_REGS_ELR_EL1   (35 * 8)
#define GZVM_REGS_SPSR(i)   (36 * 8 + (i) * 8)

#define GZVM_FPREG_OFFSET   (36 * 8 + 5 * 8 + 8)
#define GZVM_FPREG_VREG(i)  (GZVM_FPREG_OFFSET + (i) * 16)
#define GZVM_FPREG_FPSR     (GZVM_FPREG_OFFSET + 32 * 16)
#define GZVM_FPREG_FPCR     (GZVM_FPREG_OFFSET + 32 * 16 + 4)

#define GZVM_SYSREG(op0, op1, crn, crm, op2) \
    (GZVM_REG_ARM64_SYSREG | ((uint64_t)(op0) << 14) | \
     ((uint64_t)(op1) << 11) | ((uint64_t)(crn) << 7) | \
     ((uint64_t)(crm) << 3) | ((uint64_t)(op2) << 0))

static int gzvm_set_one_reg(CPUState *cs, uint64_t id, void *source)
{
    struct gzvm_one_reg reg = {
        .id = id,
        .addr = (uint64_t)(uintptr_t)source,
    };
    return gzvm_vcpu_ioctl(cs, GZVM_SET_ONE_REG, &reg);
}

int gzvm_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size)
{
    GZVMState *state = GZVM_STATE(current_accel());
    state->dtb_start = dtb_start;
    state->dtb_size = dtb_size;
    return 0;
}

void gzvm_set_gic_bases(uint64_t dist_base, uint64_t redist_base,
                        uint64_t redist_size)
{
    GZVMState *state = GZVM_STATE(current_accel());
    state->gic_dist_base = dist_base;
    state->gic_redist_base = redist_base;
    state->gic_redist_size = redist_size;
}

static int gzvm_arch_put_fpsimd(CPUState *cs)
{
    CPUARMState *env = &ARM_CPU(cs)->env;
    int i, ret;

    for (i = 0; i < 32; i++) {
        uint64_t *q = aa64_vfp_qreg(env, i);
#if HOST_BIG_ENDIAN
        uint64_t fp_val[2] = { q[1], q[0] };
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG128(GZVM_FPREG_VREG(i)),
                                fp_val);
#else
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG128(GZVM_FPREG_VREG(i)), q);
#endif
        if (ret) {
            return ret;
        }
    }
    return 0;
}

int gzvm_arch_put_registers(CPUState *cs, int level)
{
    uint64_t val;
    uint32_t fpr;
    int i, ret;
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    if (cs->cpu_index != 0) {
        /*
         * GZVM hypervisor initializes secondary VCPUs in powered-off state.
         * Only PSTATE is needed; all other registers are set by PSCI CPU_ON.
         * Writing x0-x30/PC/SP on non-boot VCPUs is rejected by hypervisor.
         * This matches crosvm's GenieZone VCPU init behavior.
         */
        val = pstate_read(env);
        return gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val);
    }

    if (!is_a64(env)) {
        aarch64_sync_32_to_64(env);
    }

    for (i = 0; i < 31; i++) {
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_X(i)),
                                &env->xregs[i]);
        if (ret) {
            error_report("gzvm    │put_registers: x%d failed (errno=%d)",
                         i, errno);
            return ret;
        }
    }

    aarch64_save_sp(env, 1);
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SP),
                            &env->sp_el[0]);
    if (ret) {
        error_report("gzvm    │put_registers: sp failed (errno=%d)", errno);
        return ret;
    }
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SP_EL1),
                            &env->sp_el[1]);
    if (ret) {
        error_report("gzvm    │put_registers: sp_el1 failed (errno=%d)", errno);
        return ret;
    }

    if (is_a64(env)) {
        val = pstate_read(env);
    } else {
        val = cpsr_read(env);
    }
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val);
    if (ret) {
        error_report("gzvm    │put_registers: pstate failed (errno=%d)", errno);
        return ret;
    }

    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PC), &env->pc);
    if (ret) {
        error_report("gzvm    │put_registers: pc failed (errno=%d)", errno);
        return ret;
    }

    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_ELR_EL1),
                            &env->elr_el[1]);
    if (ret) {
        error_report("gzvm    │put_registers: elr_el1 failed (errno=%d)", errno);
        return ret;
    }

    {
        unsigned int el = arm_current_el(env);
        if (el > 0 && !is_a64(env)) {
            i = bank_number(env->uncached_cpsr & CPSR_M);
            env->banked_spsr[i] = env->spsr;
        }
    }
    for (i = 0; i < 5; i++) {
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SPSR(i)),
                                &env->banked_spsr[i + 1]);
        if (ret) {
            error_report("gzvm    │put_registers: spsr[%d] failed (errno=%d)",
                         i, errno);
            return ret;
        }
    }

    gzvm_arch_put_fpsimd(cs);

    fpr = vfp_get_fpsr(env);
    gzvm_set_one_reg(cs, GZVM_CORE_REG32(GZVM_FPREG_FPSR), &fpr);
    fpr = vfp_get_fpcr(env);
    gzvm_set_one_reg(cs, GZVM_CORE_REG32(GZVM_FPREG_FPCR), &fpr);

    {
        uint64_t val64 = UINT64_MAX;
        ret = gzvm_set_one_reg(cs, GZVM_SYSREG(3, 3, 14, 0, 2),
                                &val64);
        if (ret) {
        }
        val64 = 0;
        gzvm_set_one_reg(cs, GZVM_SYSREG(3, 3, 14, 3, 1), &val64);
    }

    return 0;
}

static uint64_t gzvm_arm_host_features(void)
{
    return BIT(ARM_FEATURE_V8) |
           BIT(ARM_FEATURE_AARCH64) |
           BIT(ARM_FEATURE_V7) |
           BIT(ARM_FEATURE_V7VE) |
           BIT(ARM_FEATURE_GENERIC_TIMER) |
           BIT(ARM_FEATURE_NEON) |
           BIT(ARM_FEATURE_PMU);
}

void gzvm_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    ARMISARegisters *isar = &cpu->isar;
    CPUARMState *env = &cpu->env;

    SET_IDREG(isar, ID_AA64PFR0, 0x00000011);
    SET_IDREG(isar, ID_AA64PFR1, 0x00000000);
    SET_IDREG(isar, ID_AA64MMFR0, 0x00000011);
    SET_IDREG(isar, ID_AA64MMFR1, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR0, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR1, 0x00000000);

    env->features = gzvm_arm_host_features();

    cpu->midr = 0x410fd810;
    cpu->revidr = 0;
    cpu->ctr = 0x80030003;
    cpu->reset_sctlr = 0x00c50078;
    cpu->dtb_compatible = "arm,armv8";
}
