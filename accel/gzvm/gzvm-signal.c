#include "qemu/osdep.h"
#include <sys/mman.h>
#include "system/gzvm.h"
#include "gzvm-internal.h"

static uintptr_t gzvm_signal_page_size;

/*
 * Async-signal-safe SIGBUS/SIGSEGV handler for demand paging.
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


