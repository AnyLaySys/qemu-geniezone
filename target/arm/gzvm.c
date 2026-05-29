#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
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

#define GZVM_REGS_X(i)      ((i) * 8)
#define GZVM_REGS_PC        (32 * 8)
#define GZVM_REGS_PSTATE    (33 * 8)

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

static int gzvm_get_one_reg_sw(CPUState *cs, uint64_t id, void *target)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    switch (id) {
    case GZVM_CORE_REG(GZVM_REGS_PSTATE):
        *(uint64_t *)target = PSTATE_DAIF | PSTATE_MODE_EL1h;
        return 0;
    case GZVM_CORE_REG(GZVM_REGS_PC):
        *(uint64_t *)target = env->pc;
        return 0;
    case GZVM_CORE_REG(GZVM_REGS_X(0)):
        *(uint64_t *)target = env->xregs[0];
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

int gzvm_arch_get_registers(CPUState *cs, int level)
{
    /*
     * GZVM_GET_ONE_REG is not supported by the kernel driver
     * (returns -EOPNOTSUPP).  We cannot sync actual guest state
     * from the hypervisor, so this is a no-op that leaves env
     * unchanged.  The env already holds whatever we last wrote
     * via put_registers; for a running guest this is stale but
     * harmless — QEMU's monitor and GDB stub will still display
     * something rather than crashing.
     */
    return 0;
}

int gzvm_arch_put_registers(CPUState *cs, int level)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint64_t val;
    int ret;

    /*
     * Match crosvm's GenieZone backend: only write the 3 registers
     * the hypervisor needs.  Writing extra registers (FPSIMD, SPSR,
     * ELR_EL1, system registers) causes GZVM_EXIT_INTERNAL_ERROR.
     *
     * For secondary VCPUs, only PSTATE is needed; the hypervisor
     * powers them off and PSCI CPU_ON restores the rest.
     */

    /*
     * 1. PSTATE = DAIF masked | EL1h
     *
     * GenieZone hypervisor owns EL2, so the guest must run at EL1h
     * (not EL2h which QEMU's arm_emulate_firmware_reset would set).
     * This matches crosvm's PSR_D_BIT | PSR_A_BIT | PSR_I_BIT |
     * PSR_F_BIT | PSR_MODE_EL1H = 0x3C5.
     */
    val = PSTATE_DAIF | PSTATE_MODE_EL1h;
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val);
    if (ret) {
        error_report("gzvm    │put_registers: pstate failed (errno=%d)", errno);
        return ret;
    }

    if (cs->cpu_index != 0) {
        /* Secondary VCPU: hypervisor powers it off; PSCI sets the rest. */
        return 0;
    }

    /* 2. PC = kernel entry (boot.c sets env->pc = info->entry when gzvm) */
    val = env->pc;
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PC), &val);
    if (ret) {
        error_report("gzvm    │put_registers: pc failed (errno=%d)", errno);
        return ret;
    }

    /* 3. X0 = DTB address (boot.c sets env->xregs[0] = info->dtb_start) */
    val = env->xregs[0];
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_X(0)), &val);
    if (ret) {
        error_report("gzvm    │put_registers: x0 failed (errno=%d)", errno);
        return ret;
    }

    /* No FPSIMD, no SPSR, no ELR_EL1, no sysreg writes — matches crosvm */
    return 0;
}

static uint32_t gzvm_arm_read_midr(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    uint32_t midr = 0x410fd810;
    if (!f) {
        return midr;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long val;
        if (sscanf(line, "CPU implementer : 0x%lx", &val) == 1) {
            midr = (midr & ~0xff000000) | ((val & 0xff) << 24);
        } else if (sscanf(line, "CPU variant : 0x%lx", &val) == 1) {
            midr = (midr & ~0x00f00000) | ((val & 0xf) << 20);
        } else if (sscanf(line, "CPU part : 0x%lx", &val) == 1) {
            midr = (midr & ~0x000fff00) | ((val & 0xfff) << 8);
        } else if (sscanf(line, "CPU revision : %lu", &val) == 1) {
            midr = (midr & ~0x0000000f) | (val & 0xf);
        }
    }
    fclose(f);
    return midr;
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

    /*
     * Use safe ARMv8.0 defaults instead of reading MRS from the host CPU.
     * Reading host MRS values on modern cores returns SVE/SME/SVE2/BF16/etc.
     * which the GenieZone hypervisor (EL2 firmware) cannot virtualize.
     * This matches crosvm's behavior: VmCap::Sve => false.
     *
     * If the hypervisor reports different features via GZVM_CHECK_EXTENSION,
     * we could probe those here in the future.
     */

    SET_IDREG(isar, ID_AA64PFR0,  0x00000011);
    SET_IDREG(isar, ID_AA64PFR1,  0x00000000);
    SET_IDREG(isar, ID_AA64PFR2,  0x00000000);
    SET_IDREG(isar, ID_AA64DFR0,  0x00000000);
    SET_IDREG(isar, ID_AA64DFR1,  0x00000000);
    SET_IDREG(isar, ID_AA64MMFR0, 0x00000011);
    SET_IDREG(isar, ID_AA64MMFR1, 0x00000000);
    SET_IDREG(isar, ID_AA64MMFR2, 0x00000000);
    SET_IDREG(isar, ID_AA64MMFR3, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR0, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR1, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR2, 0x00000000);
    SET_IDREG(isar, ID_AA64ISAR3, 0x00000000);
    /* SVE, SME, FPFR explicitly zeroed — hypervisor doesn't virtualize these */
    SET_IDREG(isar, ID_AA64ZFR0,  0x00000000);
    SET_IDREG(isar, ID_AA64SMFR0, 0x00000000);
    SET_IDREG(isar, ID_AA64FPFR0, 0x00000000);

    env->features = gzvm_arm_host_features();
    cpu->midr = gzvm_arm_read_midr();
    cpu->revidr = 0;
    cpu->ctr = 0x80030003;
    cpu->reset_sctlr = 0x00c50078;
    cpu->dtb_compatible = "arm,armv8";
}
