#include "qemu/osdep.h"
#include <sys/mman.h>
#include "system/gzvm.h"
#include "gzvm-internal.h"

static uintptr_t gzvm_signal_page_size = 4096;

/*
 * Async-signal-safe SIGBUS/SIGSEGV handler for demand paging.
 * The GZVM hypervisor may trigger SIGBUS on pages that are not yet
 * backed; we map them on demand and return to re-execute the faulting
 * instruction.  All other signals are re-raised with the default action.
 *
 * Only async-signal-safe functions (mmap, signal, raise) are called here.
 */
static void gzvm_sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
    if (sig == SIGBUS && si->si_addr) {
        uintptr_t page_mask = ~(gzvm_signal_page_size - 1);
        uintptr_t page_addr = (uintptr_t)si->si_addr & page_mask;
#if defined(MAP_FIXED_NOREPLACE)
        int map_flags = MAP_FIXED_NOREPLACE;
#else
        int map_flags = MAP_FIXED;
#endif
        void *ret = mmap((void *)page_addr, gzvm_signal_page_size,
                         PROT_READ | PROT_WRITE,
                         map_flags | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ret != MAP_FAILED) {
            return;
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

void gzvm_install_sigsegv_handler(void)
{
    struct sigaction sa;
    sigset_t set;

    gzvm_signal_page_size = qemu_real_host_page_size();

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
