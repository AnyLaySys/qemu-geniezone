void gzvm_start_vm(void)
{
    int ret;
    GZVMState *s = GZVM_STATE(current_accel());
    MachineState *ms = MACHINE(qdev_get_machine());

    uint64_t dist_base = s->gic_dist_base ? s->gic_dist_base : 0x08000000ULL;
    uint64_t redist_base = s->gic_redist_base ? s->gic_redist_base : 0x080A0000ULL;
    uint64_t redist_size = s->gic_redist_size ? s->gic_redist_size
                         : (0x20000ULL * ms->smp.max_cpus);

    {
        struct gzvm_create_device dist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_DIST,
            .dev_addr = dist_base,
            .dev_reg_size = 0x10000,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &dist_dev);
        if (ret) {
            error_report("gzvm    │GZVM_CREATE_DEVICE VGIC_DIST failed: %s (errno=%d)",
                         strerror(errno), errno);
        } else {
            error_report("gzvm    │VGICv3 DIST created at 0x%llx",
                         (unsigned long long)dist_base);
        }
    }

    {
        struct gzvm_create_device redist_dev = {
            .dev_type = GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST,
            .dev_addr = redist_base,
            .dev_reg_size = redist_size,
        };
        ret = gzvm_vm_ioctl(GZVM_CREATE_DEVICE, &redist_dev);
        if (ret) {
            error_report("gzvm    │GZVM_CREATE_DEVICE VGIC_REDIST failed: %s (errno=%d)",
                         strerror(errno), errno);
        } else {
            error_report("gzvm    │VGICv3 REDIST created at 0x%llx size=0x%llx",
                         (unsigned long long)redist_base,
                         (unsigned long long)redist_size);
        }
    }

    {
        struct gzvm_enable_cap cap = {
            .cap = GZVM_CAP_ENABLE_IDLE,
        };
        ret = gzvm_vm_ioctl(GZVM_ENABLE_CAP, &cap);
        if (ret) {
            error_report("gzvm    │GZVM_CAP_ENABLE_IDLE not supported (ret=%d)", ret);
        } else {
            error_report("gzvm    │GZVM_CAP_ENABLE_IDLE enabled");
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
        error_report("gzvm    │SET_DTB_CONFIG addr=0x%llx size=0x%llx",
                     (unsigned long long)s->dtb_start,
                     (unsigned long long)s->dtb_size);
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            error_report("gzvm    │GZVM_SET_DTB_CONFIG failed (non-fatal): %s (errno=%d)",
                         strerror(errno), errno);
        }
        error_report("gzvm    │SET_DTB_CONFIG succeeded");
    }
}
    
