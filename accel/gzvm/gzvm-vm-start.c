#include "qemu/osdep.h"
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
        if (s->firmware_size) {
            struct gzvm_enable_cap cap = {
                .cap = GZVM_CAP_PROTECTED_VM,
                .args = { GZVM_CAP_PVM_SET_PVMFW_GPA, s->firmware_start, 0, 0, 0 },
            };
            ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
            if (ret < 0) {
                error_report("GZVM_ENABLE_CAP PVMFW_GPA failed: %s (errno=%d)",
                             strerror(errno), errno);
                return;
            }
        }

        struct gzvm_enable_cap cap = {
            .cap = GZVM_CAP_PROTECTED_VM,
            .args = { GZVM_CAP_PVM_SET_PROTECTED_VM, 0, 0, 0, 0 },
        };
        ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
        if (ret < 0) {
            error_report("GZVM_ENABLE_CAP PROTECTED_VM failed: %s (errno=%d)",
                         strerror(errno), errno);
            return;
        }
    }

    if (s->dtb_start) {
        struct gzvm_dtb_config dtb;
        dtb.dtb_addr = s->dtb_start;
        dtb.dtb_size = s->dtb_size;
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            error_report("gzvm: GZVM_SET_DTB_CONFIG failed: %s (errno=%d)",
                         strerror(errno), errno);
            return;
        }
    }

    /*
     * Periodic timer via IRQFD is a Linux-KVM-only optimisation;
     * GenieZone does not support it.  The guest runs fine without it.
     */
}
