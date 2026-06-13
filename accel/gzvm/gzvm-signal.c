#include "qemu/osdep.h"
#include <sys/mman.h>
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

static uintptr_t gzvm_signal_page_size;

/*
 * Lock-free snapshot of GZVM memory regions (HVA ranges) for use in
 * the async-signal-safe handler below.  Only modified under slots_lock
 * (via gzvm_signal_update_regions()); the signal handler reads these
 * without locks.  A stale read is benign — the worst case is either
 * an unnecessary re-raise (a GZVM SIGBUS wrongly skipped) or a small
 * window where a just-removed slot is still consulted; both are safe.
 */
#define GZVM_SIGNAL_MAX_REGIONS 64
typedef struct {
    uintptr_t start;
    uintptr_t end;
} GZVMSignalHvaRange;

static GZVMSignalHvaRange gzvm_signal_hva_ranges[GZVM_SIGNAL_MAX_REGIONS];
static int gzvm_signal_nr_hva_ranges;

void gzvm_signal_update_regions(GZVMState *s)
{
    /* Must be called under slots_lock */
    gzvm_signal_nr_hva_ranges = 0;
    for (int i = 0; i < (int)s->nr_active_slots &&
                gzvm_signal_nr_hva_ranges < GZVM_SIGNAL_MAX_REGIONS; i++) {
        gzvm_slot *slot = &s->slots[s->sorted_ids[i]];
        if (slot->mem) {
            int idx = gzvm_signal_nr_hva_ranges++;
            gzvm_signal_hva_ranges[idx].start = (uintptr_t)slot->mem;
            gzvm_signal_hva_ranges[idx].end = (uintptr_t)slot->mem +
                                              slot->size;
        }
    }
}

/*
 * Async-signal-safe SIGBUS/SIGSEGV handler for demand paging.
 *
 * The handler is installed globally (all threads see it), but the main
 * thread immediately blocks SIGBUS/SIGSEGV after installing it; worker
 * threads inherit the blocked mask via pthread_create.  Only vCPU
 * threads call gzvm_init_vcpu_sigsegv() which *unblocks* these signals,
 * so only vCPU code paths can ever trigger this handler.  The handler
 * also checks that the fault address falls inside a registered GZVM
 * memory slot; signals outside those ranges are re-raised with the
 * default action.
 *
 * The GZVM hypervisor may trigger SIGBUS on pages that are not yet
 * backed; we map them on demand and return to re-execute the faulting
 * instruction.  All other signals are re-raised with the default action.
 *
 * Only async-signal-safe functions (mmap, sigaction, raise) are called here.
 */
static void gzvm_sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
    if (sig == SIGBUS && si->si_addr) {
        uintptr_t page_mask = ~(gzvm_signal_page_size - 1);
        uintptr_t page_addr = (uintptr_t)si->si_addr & page_mask;
        int map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        void *ret;
        bool in_gzvm = false;

        /*
         * Only handle faults on addresses that fall within a registered
         * GZVM memory slot.  Signals from non-GZVM addresses are
         * re-raised with SIG_DFL below.
         */
        for (int i = 0; i < gzvm_signal_nr_hva_ranges; i++) {
            if ((uintptr_t)si->si_addr >= gzvm_signal_hva_ranges[i].start &&
                (uintptr_t)si->si_addr < gzvm_signal_hva_ranges[i].end) {
                in_gzvm = true;
                break;
            }
        }

        if (in_gzvm) {
#ifdef MAP_FIXED_NOREPLACE
            map_flags |= MAP_FIXED_NOREPLACE;
#else
            /*
             * MAP_FIXED_NOREPLACE unavailable (pre-4.17 kernel).
             * Use plain MAP_FIXED — safe here because SIGBUS targets
             * guest RAM regions that are never backed by QEMU's own
             * mappings.
             */
            map_flags |= MAP_FIXED;
#endif
            ret = mmap((void *)page_addr, gzvm_signal_page_size,
                       PROT_READ | PROT_WRITE, map_flags, -1, 0);
            if (ret != MAP_FAILED) {
                return;
            }
#ifdef MAP_FIXED_NOREPLACE
            if (errno == EEXIST) {
                return;  /* already mapped by another thread — not an error */
            }
#endif
        }
    }

    {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        sigaction(sig, &dfl, NULL);
    }
    raise(sig);
}

void gzvm_install_sigsegv_handler(void)
{
    sigset_t set;

    gzvm_signal_page_size = qemu_real_host_page_size();

#ifndef MAP_FIXED_NOREPLACE
    warn_report("gzvm: MAP_FIXED_NOREPLACE not available (kernel < 4.17), "
                "falling back to MAP_FIXED");
#endif

    /*
     * Block SIGBUS/SIGSEGV in the calling (main) thread so that only
     * vCPU threads — which call gzvm_init_vcpu_sigsegv() — can
     * trigger the demand-paging handler.  Other threads (I/O, block,
     * etc.) inherit the blocked mask via pthread_create and will
     * never accidentally deliver a GZVM SIGBUS to this handler.
     */
    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

void gzvm_init_vcpu_sigsegv(void)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gzvm_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}


