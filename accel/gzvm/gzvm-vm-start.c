#include "qemu/osdep.h"
#include <sys/timerfd.h>
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

void gzvm_start_vm(void)
{
    int ret;
    GZVMState *s = GZVM_STATE(current_accel());

    {
        struct gzvm_enable_cap cap = {
            .cap = GZVM_CAP_ENABLE_IDLE,
        };
        ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
        if (ret) {
            error_report("gzvm: GZVM_CAP_ENABLE_IDLE not supported (ret=%d)", ret);
        }
    }

    if (s->protected_vm) {
        struct gzvm_enable_cap cap = {
            .cap = GZVM_CAP_PROTECTED_VM,
            .args = { GZVM_CAP_PVM_SET_PROTECTED_VM, 0, 0, 0, 0 },
        };
        ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
        if (ret < 0) {
            error_report("GZVM_ENABLE_CAP PROTECTED_VM failed: %s (errno=%d)",
                         strerror(errno), errno);
            exit(1);
        }
    }

    if (s->dtb_start) {
        struct gzvm_dtb_config dtb;
        dtb.dtb_addr = s->dtb_start;
        dtb.dtb_size = s->dtb_size;
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            error_report("gzvm: GZVM_SET_DTB_CONFIG failed: %s (errno=%d) — aborting",
                         strerror(errno), errno);
            exit(1);
        }
    }

    /* Set up periodic timer via IRQFD to wake VCPU from WFI */
    {
        int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        struct gzvm_irqfd irqfd;

        if (timer_fd < 0) {
            warn_report("gzvm: timerfd_create failed (errno=%d); "
                        "timer wakeup disabled", errno);
            return;
        }

        struct itimerspec ts = {
            .it_interval = { .tv_sec = 0, .tv_nsec = 5000000 },
            .it_value = { .tv_sec = 0, .tv_nsec = 1000000 },
        };

        timerfd_settime(timer_fd, 0, &ts, NULL);

        /* Try raw PPI number as GSI */
        irqfd = (struct gzvm_irqfd){
            .fd = timer_fd,
            .gsi = 27,
        };
        ret = gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
        if (ret) {
            /* Try GSI=0 (CPU IRQ line) */
            irqfd.gsi = 0;
            ret = gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
        }
        if (ret) {
            warn_report("gzvm: GZVM_IRQFD not supported (errno=%d); "
                        "timer wakeup disabled", errno);
            close(timer_fd);
            s->irqfd_timer_fd = -1;
        } else {
            warn_report("gzvm: IRQFD timer set up on GSI=%d", irqfd.gsi);
            s->irqfd_timer_fd = timer_fd;
        }
    }
}
