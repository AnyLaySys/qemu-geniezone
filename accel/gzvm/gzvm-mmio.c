#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "gzvm-internal.h"
#include "trace.h"

/*
 * Determine the correct MemTxAttrs for an MMIO access.
 * Use our O(log n) slot lookup instead of address_space_translate
 * to avoid a full memory-tree walk.  Backed slots (slot->mem != NULL)
 * are RAM/ROM and work with UNSPECIFIED; everything else (device MMIO
 * or unmapped) needs secure attributes on GZVM.
 */
static MemTxAttrs gzvm_mmio_attrs(hwaddr addr)
{
    gzvm_slot *slot = gzvm_find_slot_by_addr(addr);
    if (slot && slot->mem) {
        return MEMTXATTRS_UNSPECIFIED;
    }
    return (MemTxAttrs) { .secure = true };
}

/*
 * Caller must hold slots_lock.  On success, *slot_addr_out is set to
 * the (possibly IPA-corrected) address and the slot pointer is returned.
 */
static gzvm_slot *gzvm_find_slot_for_mmio_locked(GZVMState *s, hwaddr addr,
                                                   hwaddr *slot_addr_out)
{
    gzvm_slot *slot = gzvm_find_slot_by_addr_locked(s, addr);
    if (slot) {
        *slot_addr_out = addr;
        return slot;
    }

    /*
     * GZVM_IPA_WORKAROUND (compile-time): some firmware/platforms
     * generate MMIO exits with an IPA having bit-30 cleared and
     * bit-26 set instead of the expected address.  Remap those
     * patterns to the correct IPA when a matching slot exists.
     * Remove once the platform firmware is fixed.
     */
#if defined(GZVM_IPA_WORKAROUND)
    if ((addr >> 28) == 0x4) {
        hwaddr corrected = (addr & 0x0FFFFFFF) | 0x04000000;
        slot = gzvm_find_slot_by_addr_locked(s, corrected);
        if (slot) {
            *slot_addr_out = corrected;
            warn_report_once("gzvm: MMIO IPA corrected from 0x%" PRIx64 " to 0x%" PRIx64 " (bit-30/bit-26 workaround)", addr, corrected);
            return slot;
        }
    }
#endif
    return NULL;
}

int gzvm_handle_mmio_exit(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    hwaddr addr = run->mmio.phys_addr;
    MemTxResult r;

    if (run->mmio.size > 8) {
        warn_report("gzvm: large MMIO %s at 0x%" PRIx64 " size=%" PRIu64
                     " (max 8 bytes supported, treated as RAZ/WI)",
                     run->mmio.is_write ? "write" : "read",
                     (uint64_t)run->mmio.phys_addr,
                     (uint64_t)run->mmio.size);
        return 0;
    }

    r = address_space_rw(&address_space_memory, addr,
                         gzvm_mmio_attrs(addr),
                         run->mmio.data, run->mmio.size, run->mmio.is_write);
    if (r == MEMTX_OK) {
        return 0;
    }

    {
        AccelState *accel = current_accel();
        GZVMState *s = accel ? GZVM_STATE(accel) : NULL;
        if (s) {
            gzvm_slots_lock(s);
            hwaddr slot_addr;
            gzvm_slot *slot = gzvm_find_slot_for_mmio_locked(s, addr,
                                                              &slot_addr);
            if (slot) {
                uint64_t offset = slot_addr - slot->start;
                if (offset < slot->size) {
                    size_t xlen = MIN((uint64_t)run->mmio.size,
                                      slot->size - offset);
                    trace_gzvm_mmio_fallback(addr, run->mmio.size,
                                             run->mmio.is_write);
                    if (run->mmio.is_write) {
                        memcpy(slot->mem + offset, run->mmio.data, xlen);
                    } else {
                        memcpy(run->mmio.data, slot->mem + offset, xlen);
                    }
                    gzvm_slots_unlock(s);
                    return 0;
                }
            }
            gzvm_slots_unlock(s);
        }
    }

    warn_report("gzvm: %s at 0x%" PRIx64 " size=%" PRIu64 " returned %u, "
                "treated as RAZ/WI",
                run->mmio.is_write ? "MMIO write" : "MMIO read",
                (uint64_t)run->mmio.phys_addr,
                (uint64_t)run->mmio.size, r);
    return 0;
}

int gzvm_handle_system_event(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    trace_gzvm_handle_system_event(run->system_event.type);
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
        cpu->halted = 0;
        return EXCP_INTERRUPT;
    case GZVM_SYSTEM_EVENT_SUSPEND:
    case GZVM_SYSTEM_EVENT_S2IDLE:
        cpu->halted = 1;
        return EXCP_INTERRUPT;
    case GZVM_SYSTEM_EVENT_SEV_TERM:
        warn_report("gzvm: SEV_TERM event on VCPU%u (not applicable to GZVM)",
                    cpu->cpu_index);
        return EXCP_INTERRUPT;
    default:
        return 0;
    }
}

int gzvm_handle_fail_entry(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm: CPU#%d FAIL_ENTRY reason=0x%" PRIx64 " cpu=%u",
                 cpu->cpu_index,
                 (uint64_t)run->fail_entry.hardware_entry_failure_reason,
                 run->fail_entry.cpu);
    return -1;
}

int gzvm_handle_internal_error(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm: CPU#%d INTERNAL_ERROR suberror=%u ndata=%u",
                 cpu->cpu_index, run->internal.suberror, run->internal.ndata);
    for (int i = 0; i < run->internal.ndata && i < 16; i++) {
        error_report("gzvm:   data[%d] = 0x%" PRIx64, i,
                     (uint64_t)run->internal.data[i]);
    }
    return -1;
}

int gzvm_handle_unknown_exit(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    error_report("gzvm: CPU#%d unknown exit_reason=0x%x",
                 cpu->cpu_index, run->exit_reason);
    return 0;
}
