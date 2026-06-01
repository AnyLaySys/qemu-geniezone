#ifndef GZVM_INT_H
#define GZVM_INT_H

#include "qemu/accel.h"
#include "qemu/typedefs.h"
#include "qemu/thread.h"
#include "accel/accel-ops.h"
#include "linux-headers/linux/gzvm.h"

typedef struct gzvm_slot {
    uint64_t start;
    uint64_t size;
    uint8_t *mem;
    uint32_t id;
    uint32_t flags;
} gzvm_slot;

#define GZVM_MAX_MEM_SLOTS    512

struct GZVMState {
    AccelState parent_obj;
    QemuMutex slots_lock;
    gzvm_slot slots[GZVM_MAX_MEM_SLOTS];
    uint32_t nr_slots;
    GList *free_slots;
    int fd;
    int vmfd;
    bool protected_vm;
    uint64_t dtb_start;
    uint64_t dtb_size;
    uint64_t gic_dist_base;
    uint64_t gic_redist_base;
    uint64_t gic_redist_size;
    uint64_t ram_base;
};

struct GZVCPUState {
    int fd;
    struct gzvm_vcpu_run *run;
    uint64_t last_fault_addr;
    int same_fault_count;
    uint64_t last_mmio_addr;
    uint64_t last_mmio_data;
    uint8_t last_mmio_size;
    uint8_t last_mmio_valid;
};

#define GZVCPU(cpu) ((struct GZVCPUState *)(cpu)->accel)

int gzvm_create_vm(void);
void gzvm_start_vm(void);
int gzvm_vm_ioctl(int type, void *arg);
int gzvm_vcpu_ioctl(CPUState *cpu, int type, void *arg);
void *gzvm_cpu_thread_fn(void *arg);
int gzvm_add_irqfd(int irqfd, int gsi, bool resample, Error **errp);
int gzvm_arch_put_registers(CPUState *cs, int level);
int gzvm_arch_get_registers(CPUState *cs, int level);
void gzvm_cpu_synchronize_post_reset(CPUState *cpu);
gzvm_slot *gzvm_find_slot_by_addr(uint64_t addr);

#endif
