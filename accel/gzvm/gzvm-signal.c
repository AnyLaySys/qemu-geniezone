#include "qemu/osdep.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "gzvm-internal.h"

__attribute__((constructor))
static void gzvm_unblock_sigbus_early(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

static void gzvm_sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    if (sig == SIGBUS && si->si_addr) {
        uintptr_t page_addr = (uintptr_t)si->si_addr & ~(uintptr_t)0xFFF;
#if defined(MAP_FIXED_NOREPLACE)
        int map_flags = MAP_FIXED_NOREPLACE;
#else
        int map_flags = MAP_FIXED;
#endif
        void *ret = mmap((void *)page_addr, 4096, PROT_READ | PROT_WRITE,
                         map_flags | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ret != MAP_FAILED) {
            return;
        }
    }
    {
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
                           "\n=== GZVM SIGNAL ===\n"
                           "Signal: %d (%s)\n"
                           "Faulting address: %p\n"
                           "si_code: %d\n",
                           sig,
                           sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "?",
                           si->si_addr, si->si_code);
        if (len > 0) {
            volatile int wret = write(STDERR_FILENO, buf, len);
            (void)wret;
        }
    }
    if (uc) {
        char buf[128];
#if defined(__aarch64__)
        int len = snprintf(buf, sizeof(buf), "PC=0x%" PRIx64 "\n",
                            (uint64_t)uc->uc_mcontext.pc);
#else
        int len = snprintf(buf, sizeof(buf), "PC unavailable on this host\n");
#endif
        if (len > 0) {
            volatile int wret = write(STDERR_FILENO, buf, len);
            (void)wret;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void gzvm_install_sigsegv_handler(void)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gzvm_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}
