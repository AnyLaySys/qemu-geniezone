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

static gzvm_slot *gzvm_find_slot_for_mmio(hwaddr addr, hwaddr *slot_addr_out)
{
    gzvm_slot *slot = gzvm_find_slot_by_addr(addr);
    if (slot) {
        *slot_addr_out = addr;
        return slot;
    }
    /* Hypervisor may misreport IPA: bit 30 instead of bit 26 */
    if ((addr >> 28) == 0x4) {
        hwaddr corrected = (addr & 0x0FFFFFFF) | 0x04000000;
        slot = gzvm_find_slot_by_addr(corrected);
        if (slot) {
            *slot_addr_out = corrected;
            return slot;
        }
    }
    return NULL;
}

int gzvm_handle_mmio_exit(CPUState *cpu, struct gzvm_vcpu_run *run)
{
    hwaddr addr = run->mmio.phys_addr;
    MemTxResult r;

    r = address_space_rw(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                             run->mmio.data, run->mmio.size, run->mmio.is_write);
    
    if (r == MEMTX_OK) {
        return 0;
    }

    /* 
     * If basic RW fails, we treat this as a device access.
     * We fallback to a more permissive attribute set to ensure GPU/Device 
     * registers can be reached.
     */
    r = address_space_rw(&address_space_memory, addr,
                          (MemTxAttrs) { .secure = true },
                          run->mmio.data, run->mmio.size, run->mmio.is_write);
    if (r == MEMTX_OK) {
        return 0;
    }

    if (run->mmio.size > 8) {
        return 0;
    }

    {
        hwaddr slot_addr;
        gzvm_slot *slot = gzvm_find_slot_for_mmio(addr, &slot_addr);
        if (slot) {
            uint64_t offset = slot_addr - slot->start;
            if (offset < slot->size) {
                size_t xlen = MIN((uint64_t)run->mmio.size,
                                  slot->size - offset);
                if (run->mmio.is_write) {
                    memcpy(slot->mem + offset, run->mmio.data, xlen);
                } else {
                    memcpy(run->mmio.data, slot->mem + offset, xlen);
                }
                return 0;
            }
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
