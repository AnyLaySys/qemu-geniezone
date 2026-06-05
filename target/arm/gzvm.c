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
#include "cpu-sysregs.h"

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
#define GZVM_REGS_SP_EL1    (34 * 8)  /* offsetof(gzvm_regs, sp_el1)  = 272 */
#define GZVM_REGS_ELR_EL1   (35 * 8)  /* offsetof(gzvm_regs, elr_el1) = 280 */
#define GZVM_REGS_SPSR(i)   (36 * 8 + (i) * 8)

#define GZVM_FPREG_OFFSET   (36 * 8 + 5 * 8 + 8)
#define GZVM_FPREG_VREG(i)  (GZVM_FPREG_OFFSET + (i) * 16)
#define GZVM_FPREG_FPSR     (GZVM_FPREG_OFFSET + 32 * 16)
#define GZVM_FPREG_FPCR     (GZVM_FPREG_OFFSET + 32 * 16 + 4)

#define GZVM_SYSREG(op0, op1, crn, crm, op2) \
    (GZVM_REG_ARM64_SYSREG | ((uint64_t)(op0) << 14) | \
     ((uint64_t)(op1) << 11) | ((uint64_t)(crn) << 7) | \
     ((uint64_t)(crm) << 3) | ((uint64_t)(op2) << 0))

#define DEF(NAME, OP0, OP1, CRN, CRM, OP2) [NAME##_IDX] = #NAME,
static const char * const gzvm_id_reg_names[NUM_ID_IDX] = {
#include "cpu-sysregs.h.inc"
};
#undef DEF

static int gzvm_set_one_reg(CPUState *cs, uint64_t id, void *source);

static bool gzvm_id_regs_written;

static void gzvm_arch_set_id_regs(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    int i;

    if (gzvm_id_regs_written) {
        return;
    }

    for (i = 0; i < NUM_ID_IDX; i++) {
        uint64_t reg = cpu->isar.idregs[i];
        uint64_t sysid;

        if (!reg) {
            continue;
        }
        sysid = GZVM_REG_ARM64_SYSREG |
                (id_register_sysreg[i] & 0x3fff);
        gzvm_set_one_reg(cs, sysid, &reg);
    }
    gzvm_id_regs_written = true;
}

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

    /*
     * Kernel driver creates VGIC DIST/REDIST devices with hardcoded base
     * addresses (0x08000000 / 0x080A0000) during gzvm_create_vm() and
     * ignores the dev_addr fields.  If the machine memory map differs,
     * the kernel and QEMU will be out of sync — warn here.
     */
    if (dist_base != 0x08000000ULL) {
        warn_report("gzvm    │machine DIST base 0x%" PRIx64
                    " != kernel hardcoded 0x08000000; GIC may not work",
                    dist_base);
    }
    if (redist_base != 0x080A0000ULL) {
        warn_report("gzvm    │machine REDIST base 0x%" PRIx64
                    " != kernel hardcoded 0x080A0000; GIC may not work",
                    redist_base);
    }
}

void gzvm_set_ram_base(uint64_t base)
{
    GZVMState *state = GZVM_STATE(current_accel());
    state->ram_base = base;
}

static int gzvm_get_one_reg_sw(CPUState *cs, uint64_t id, void *target)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    switch (id) {
    case GZVM_CORE_REG(GZVM_REGS_PSTATE):
        *(uint64_t *)target = env->pstate;
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

int gzvm_arch_get_registers(CPUState *cs, int level)
{
    /*
     * GZVM_GET_ONE_REG is not supported by the kernel driver
     * (returns -EOPNOTSUPP).  Read from env as a best-effort
     * fallback so 'info registers' and GDB show something
     * rather than zeroes or crashing.
     */
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint64_t val;

    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val) == 0) {
        env->pstate = val;
    }
    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_PC), &val) == 0) {
        env->pc = val;
    }
    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_X(0)), &val) == 0) {
        env->xregs[0] = val;
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
        val = pstate_read(env);
        return gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val);
    }

    gzvm_arch_set_id_regs(cs);

    if (!is_a64(env)) {
        aarch64_sync_32_to_64(env);
    }

    for (i = 0; i < 31; i++) {
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_X(i)),
                                &env->xregs[i]);
        if (ret) {
            return ret;
        }
    }

    aarch64_save_sp(env, 1);
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SP),
                            &env->sp_el[0]);
    if (ret) {
        return ret;
    }
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SP_EL1),
                            &env->sp_el[1]);
    if (ret) {
        return ret;
    }

    if (is_a64(env)) {
        val = pstate_read(env);
    } else {
        val = cpsr_read(env);
    }
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val);
    if (ret) {
        return ret;
    }

    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_PC), &env->pc);
    if (ret) {
        return ret;
    }

    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_ELR_EL1),
                            &env->elr_el[1]);
    if (ret) {
        return ret;
    }

    {
        unsigned int el = arm_current_el(env);
        if (el > 0 && !is_a64(env)) {
            i = bank_number(env->uncached_cpsr & CPSR_M);
            env->banked_spsr[i] = env->spsr;
        }
    }
    for (i = 0; i < GZVM_NR_SPSR; i++) {
        ret = gzvm_set_one_reg(cs, GZVM_CORE_REG(GZVM_REGS_SPSR(i)),
                                &env->banked_spsr[i + 1]);
        if (ret) {
            return ret;
        }
    }

    ret = gzvm_arch_put_fpsimd(cs);
    if (ret) {
        return ret;
    }

    fpr = vfp_get_fpsr(env);
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG32(GZVM_FPREG_FPSR), &fpr);
    if (ret) {
        return ret;
    }
    fpr = vfp_get_fpcr(env);
    ret = gzvm_set_one_reg(cs, GZVM_CORE_REG32(GZVM_FPREG_FPCR), &fpr);
    if (ret) {
        return ret;
    }

    {
        uint64_t val64 = UINT64_MAX;
        ret = gzvm_set_one_reg(cs, GZVM_SYSREG(3, 3, 14, 0, 2), &val64);
        if (ret) {
            return ret;
        }
        val64 = 0;
        ret = gzvm_set_one_reg(cs, GZVM_SYSREG(3, 3, 14, 3, 1), &val64);
        if (ret) {
            return ret;
        }
    }

    return 0;
}

static uint32_t gzvm_arm_read_midr(void)
{
    static uint32_t cached_midr;
    static bool cached;
    FILE *f;
    uint32_t midr = 0x410fd810;
    char line[256];

    if (cached) {
        return cached_midr;
    }

    f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        cached_midr = midr;
        cached = true;
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
    cached_midr = midr;
    cached = true;
    return midr;
}

static bool gzvm_read_host_sysreg(const char *name, uint64_t *value)
{
    char *lower = g_ascii_strdown(name, -1);
    char *path = g_strdup_printf(
        "/sys/devices/system/cpu/cpu0/regs/identification/%s", lower);
    char *contents = NULL;
    char *end;
    bool ok = false;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        *value = g_ascii_strtoull(contents, &end, 0);
        ok = end != contents;
    }

    g_free(contents);
    g_free(path);
    g_free(lower);
    return ok;
}

/*
 * Fallback: read identification register directly via MRS from userspace.
 * On arm64 the ID_AA64* and other identification registers are accessible
 * at EL0 when the kernel exposes the 'cpuid' hwcap.  Use .inst with the
 * raw encoding to bypass assembler validation of newer register names.
 *
 * Some registers may not exist on older CPU implementations and will
 * trigger SIGILL.  We catch the signal with sigsetjmp/siglongjmp so
 * that unavailable registers are silently skipped.
 *
 * MRS instruction encoding: 0xD5200000 | (sysreg_enc << 5) | Rt
 * where sysreg_enc = (op0 << 14) | (op1 << 11) | (CRn << 7) | (CRm << 3) | op2
 * and Rt=0 (X0).  The variable v is pinned to x0 to match.
 */
static sigjmp_buf gzvm_sysreg_jmp;

static void gzvm_sysreg_sigill(int sig)
{
    siglongjmp(gzvm_sysreg_jmp, 1);
}

static bool gzvm_read_sysreg_direct(int idx, uint64_t *value)
{
    struct sigaction sa, old;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gzvm_sysreg_sigill;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, &old);

    if (sigsetjmp(gzvm_sysreg_jmp, 1) == 0) {
        switch (idx) {
#define DEF(NAME, OP0, OP1, CRN, CRM, OP2) \
        case NAME##_IDX: { \
            register uint64_t v asm("x0"); \
             asm volatile(".inst " \
                stringify(0xd5200000 | ((((OP0 << 14) | (OP1 << 11) | (CRN << 7) | (CRM << 3) | OP2) << 5) | 0)) \
                : "=r"(v)); \
            *value = v; \
            sigaction(SIGILL, &old, NULL); \
            return true; \
        }
#include "cpu-sysregs.h.inc"
#undef DEF
        default:
            break;
        }
    }

    sigaction(SIGILL, &old, NULL);
    return false;
}

static uint64_t gzvm_arm_host_features_from_idregs(ARMISARegisters *isar)
{
    uint64_t features = BIT(ARM_FEATURE_V8) |
                        BIT(ARM_FEATURE_AARCH64) |
                        BIT(ARM_FEATURE_V7) |
                        BIT(ARM_FEATURE_V7VE) |
                        BIT(ARM_FEATURE_GENERIC_TIMER);
    uint64_t pfr0 = GET_IDREG(isar, ID_AA64PFR0);
    uint64_t dfr0 = GET_IDREG(isar, ID_AA64DFR0);

    if (FIELD_EX64(pfr0, ID_AA64PFR0, ADVSIMD) != 0xf) {
        features |= BIT(ARM_FEATURE_NEON);
    }
    if (FIELD_EX64(pfr0, ID_AA64PFR0, EL2) != 0) {
        features |= BIT(ARM_FEATURE_EL2);
    }
    if (FIELD_EX64(pfr0, ID_AA64PFR0, EL3) != 0) {
        features |= BIT(ARM_FEATURE_EL3);
    }
    if (FIELD_EX64(dfr0, ID_AA64DFR0, PMUVER) != 0 &&
        FIELD_EX64(dfr0, ID_AA64DFR0, PMUVER) != 0xf) {
        features |= BIT(ARM_FEATURE_PMU);
    }

    return features;
}

static bool gzvm_arm_read_host_cpu_features(ARMCPU *cpu)
{
    ARMISARegisters *isar = &cpu->isar;
    uint64_t value;
    int read = 0;

    for (int i = 0; i < NUM_ID_IDX; i++) {
        if (gzvm_id_reg_names[i] &&
            (gzvm_read_host_sysreg(gzvm_id_reg_names[i], &value) ||
             gzvm_read_sysreg_direct(i, &value))) {
            isar->idregs[i] = value;
            read++;
        }
    }

    if (!read || !GET_IDREG(isar, ID_AA64PFR0)) {
        return false;
    }

    isar->mvfr0 = (uint32_t)GET_IDREG(isar, MVFR0);
    isar->mvfr1 = (uint32_t)GET_IDREG(isar, MVFR1);
    isar->mvfr2 = (uint32_t)GET_IDREG(isar, MVFR2);

    if (gzvm_read_host_sysreg("MIDR_EL1", &value)) {
        cpu->midr = value;
    } else {
        cpu->midr = gzvm_arm_read_midr();
    }
    if (gzvm_read_host_sysreg("REVIDR_EL1", &value)) {
        cpu->revidr = value;
    }
    if (gzvm_read_host_sysreg("CTR_EL0", &value)) {
        cpu->ctr = value;
    } else {
        cpu->ctr = isar->idregs[CTR_EL0_IDX];
    }

    return true;
}

void gzvm_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    ARMISARegisters *isar = &cpu->isar;
    CPUARMState *env = &cpu->env;

    if (!gzvm_arm_read_host_cpu_features(cpu)) {
        cpu->host_cpu_probe_failed = true;
        return;
    }

    env->features = gzvm_arm_host_features_from_idregs(isar);
    cpu->reset_sctlr = 0x00c50078;
    cpu->dtb_compatible = "arm,armv8";

    /*
     * Kernel sanitizes ID_AA64MMFR0_EL1.PARANGE for EL0 MRS accesses
     * (returns 0 = 32-bit).  Query GZVM's actual IPA capability and
     * override PARANGE so arm_pamax() returns the correct value.
     */
    {
        uint64_t cap = GZVM_CAP_ARM_VM_IPA_SIZE;
        int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION, &cap);
        if (r == 0 && cap > 0) {
            unsigned int gzvm_parange = round_down_to_parange_index(cap);
            unsigned int cur_parange =
                FIELD_EX64_IDREG(isar, ID_AA64MMFR0, PARANGE);
            if (gzvm_parange > cur_parange) {
                uint64_t mmfr0 = GET_IDREG(isar, ID_AA64MMFR0);
                mmfr0 = FIELD_DP64(mmfr0, ID_AA64MMFR0, PARANGE, gzvm_parange);
                SET_IDREG(isar, ID_AA64MMFR0, mmfr0);
            }
        }
    }

    if (cpu_isar_feature(aa64_sve, cpu)) {
        cpu->sve_vq.supported = MAKE_64BIT_MASK(0, ARM_MAX_VQ);
    }
    if (cpu_isar_feature(aa64_sme, cpu)) {
        cpu->sme_vq.supported = SVE_VQ_POW2_MAP;
    }
}
