void gzvm_start_vm(void)
{
    int ret;
    GZVMState *s = GZVM_STATE(current_accel());

    error_report("gzvm    │GIC DIST+REDIST already created early (pre-VCPU)");
    error_report("gzvm    │DIST base=0x%llx  REDIST base=0x%llx",
                 (unsigned long long)s->gic_dist_base,
                 (unsigned long long)s->gic_redist_base);

    error_report("gzvm    │GZVM_CAP_ENABLE_IDLE skipped (crosvm doesn't use it)");

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
        error_report("gzvm    │SET_DTB_CONFIG addr=0x%llx size=0x%llx",
                     (unsigned long long)s->dtb_start,
                     (unsigned long long)s->dtb_size);
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            error_report("gzvm    │GZVM_SET_DTB_CONFIG failed: %s (errno=%d) — aborting",
                         strerror(errno), errno);
            exit(1);
        }
        error_report("gzvm    │SET_DTB_CONFIG succeeded");
    }
}
    
