#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"
#include "trace.h"

void gzvm_start_vm(void)
{
    AccelState *accel = current_accel();
    GZVMState *s;
    int ret;

    if (!accel) {
        return;
    }
    s = GZVM_STATE(accel);

    trace_gzvm_start_vm(s->gic_dist_base, s->gic_redist_base);

    if (s->protected_vm) {
        if (s->firmware_size) {
            struct gzvm_enable_cap cap = {
                .cap = GZVM_CAP_PROTECTED_VM,
                .args = { GZVM_CAP_PVM_SET_PVMFW_GPA, s->firmware_start, 0, 0, 0 },
            };
            ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
            if (ret < 0) {
                error_report("gzvm: GZVM_ENABLE_CAP PVMFW_GPA failed: %s (errno=%d)",
                             strerror(errno), errno);
                exit(1);
            }
        }

        struct gzvm_enable_cap cap = {
            .cap = GZVM_CAP_PROTECTED_VM,
            .args = { GZVM_CAP_PVM_SET_PROTECTED_VM, 0, 0, 0, 0 },
        };
        ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
        if (ret < 0) {
            error_report("gzvm: GZVM_ENABLE_CAP PROTECTED_VM failed: %s (errno=%d)",
                         strerror(errno), errno);
            exit(1);
        }
    }

    if (s->dtb_start) {
        struct gzvm_dtb_config dtb;
        dtb.dtb_addr = s->dtb_start;
        dtb.dtb_size = s->dtb_size;
        trace_gzvm_set_dtb_config(dtb.dtb_addr, dtb.dtb_size);
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            error_report("gzvm: GZVM_SET_DTB_CONFIG failed: %s (errno=%d)",
                         strerror(errno), errno);
            exit(1);
        }
    }
}
