int gzvm_add_irqfd(int irqfd, int gsi, bool resample, Error **errp)
{
    int ret;
    struct gzvm_irqfd gzirqfd;

    memset(&gzirqfd, 0, sizeof(gzirqfd));
    gzirqfd.fd = irqfd;
    gzirqfd.gsi = gsi;
    if (resample) {
        gzirqfd.flags = GZVM_IRQFD_FLAG_RESAMPLE;
    }

    ret = gzvm_vm_ioctl(GZVM_IRQFD, &gzirqfd);
    if (ret) {
        error_setg_errno(errp, errno, "GZVM_IRQFD failed");
    }
    return ret;
}

static int
gzvm_set_ioeventfd_mmio(int fd, hwaddr addr, uint32_t size, uint64_t data,
                        bool datamatch, bool assign)
{
    int ret;
    struct gzvm_ioeventfd io;

    memset(&io, 0, sizeof(io));
    io.fd = fd;
    io.datamatch = datamatch ? data : 0;
    io.len = size;
    io.addr = addr;
    io.flags = datamatch ? GZVM_IOEVENTFD_FLAG_DATAMATCH : 0;
    if (!assign) {
        io.flags |= GZVM_IOEVENTFD_FLAG_DEASSIGN;
    }

    ret = gzvm_vm_ioctl(GZVM_IOEVENTFD, &io);
    return ret;
}

static void
gzvm_mem_ioeventfd_add(MemoryListener *listener, MemoryRegionSection *section,
                        bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                 int128_get64(section->size), data,
                                 match_data, true);
    if (r < 0 && errno == EEXIST) {
        gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                 int128_get64(section->size), data,
                                 match_data, false);
        r = gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                     int128_get64(section->size), data,
                                     match_data, true);
    }
    if (r < 0) {
        error_report("gzvm    │ioeventfd_add failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

static void
gzvm_mem_ioeventfd_del(MemoryListener *listener, MemoryRegionSection *section,
                        bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);

    gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                             int128_get64(section->size), data,
                             match_data, false);
}

MemoryListener gzvm_ioeventfd_listener = {
    .name = "gzvm-ioeventfd",
    .eventfd_add = gzvm_mem_ioeventfd_add,
    .eventfd_del = gzvm_mem_ioeventfd_del,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};
