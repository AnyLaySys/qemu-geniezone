#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/memory.h"
#include "qemu/event_notifier.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "gzvm-internal.h"
#include "trace/trace-accel_gzvm.h"

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
        /* Already registered with the same parameters; nothing to do */
        return;
    }
    if (r < 0) {
        error_report("gzvm: ioeventfd_add failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

static void
gzvm_mem_ioeventfd_del(MemoryListener *listener, MemoryRegionSection *section,
                        bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                 int128_get64(section->size), data,
                                 match_data, false);
    if (r < 0 && errno != ENOENT) {
        error_report("gzvm: ioeventfd_del failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

int gzvm_add_irqfd(EventNotifier *n, EventNotifier *rn, int gsi)
{
    struct gzvm_irqfd irqfd = {
        .fd = event_notifier_get_fd(n),
        .gsi = gsi,
        .flags = 0,
    };

    if (rn) {
        irqfd.flags |= GZVM_IRQFD_FLAG_RESAMPLE;
        irqfd.resamplefd = event_notifier_get_fd(rn);
    }

    trace_gzvm_add_irqfd(irqfd.fd, gsi);
    return gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
}

int gzvm_remove_irqfd(EventNotifier *n, int gsi)
{
    struct gzvm_irqfd irqfd = {
        .fd = event_notifier_get_fd(n),
        .gsi = gsi,
        .flags = GZVM_IRQFD_FLAG_DEASSIGN,
    };

    trace_gzvm_add_irqfd(irqfd.fd, gsi);
    return gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
}

MemoryListener gzvm_ioeventfd_listener = {
    .name = "gzvm-ioeventfd",
    .eventfd_add = gzvm_mem_ioeventfd_add,
    .eventfd_del = gzvm_mem_ioeventfd_del,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};
