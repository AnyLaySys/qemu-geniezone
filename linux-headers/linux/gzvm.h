/*
SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note 
*/
#ifndef __LINUX_GZVM_H__
#define __LINUX_GZVM_H__

#include <stdint.h>
#include <sys/ioctl.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <linux/types.h>
#else
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
#endif


#ifndef FIELD_PREP
#define __LOW32(x) ((uint32_t)(x))
#define __CTZ(x) (__builtin_ctzll(x))
#define GENMASK_ULL(h, l) (((~0ULL) >> (63 - (h))) << (l))
#define GENMASK(h, l) (((~(__LOW32(0))) >> (31 - (h))) << (l))
#define FIELD_PREP(mask, val) (((uint64_t)(val) << __CTZ(mask)) & (mask))
#endif

#define GZVM_IOC_MAGIC			0x92

#define GZVM_CAP_VM_GPA_SIZE		0xa5
#define GZVM_CAP_PROTECTED_VM		0xffbadab1
#define GZVM_CAP_BLOCK_BASED_DEMAND_PAGING	0x9201
#define GZVM_CAP_ENABLE_DEMAND_PAGING	0x9202
#define GZVM_CAP_ENABLE_IDLE		0x9203
#define GZVM_CAP_QUERY_HYP_BATCH_PAGES	0x9204
#define GZVM_CAP_QUERY_DESTROY_BATCH_PAGES	0x9205

#define GZVM_CAP_ARM_VM_IPA_SIZE	GZVM_CAP_VM_GPA_SIZE
#define GZVM_CAP_ARM_PROTECTED_VM	GZVM_CAP_PROTECTED_VM

#define GZVM_CAP_PVM_SET_PVMFW_GPA		0
#define GZVM_CAP_PVM_GET_PVMFW_SIZE		1
#define GZVM_CAP_PVM_SET_PROTECTED_VM		2

#define GZVM_REG_ARCH_ARM64	FIELD_PREP(GENMASK_ULL(63, 56), 0x60)
#define GZVM_REG_ARCH_MASK	FIELD_PREP(GENMASK_ULL(63, 56), 0xff)

#define GZVM_REG_SIZE_SHIFT	52
#define GZVM_REG_SIZE_MASK	FIELD_PREP(GENMASK_ULL(63, 48), 0x00f0)

#define GZVM_REG_SIZE_U8	FIELD_PREP(GENMASK_ULL(63, 48), 0x0000)
#define GZVM_REG_SIZE_U16	FIELD_PREP(GENMASK_ULL(63, 48), 0x0010)
#define GZVM_REG_SIZE_U32	FIELD_PREP(GENMASK_ULL(63, 48), 0x0020)
#define GZVM_REG_SIZE_U64	FIELD_PREP(GENMASK_ULL(63, 48), 0x0030)
#define GZVM_REG_SIZE_U128	FIELD_PREP(GENMASK_ULL(63, 48), 0x0040)
#define GZVM_REG_SIZE_U256	FIELD_PREP(GENMASK_ULL(63, 48), 0x0050)
#define GZVM_REG_SIZE_U512	FIELD_PREP(GENMASK_ULL(63, 48), 0x0060)
#define GZVM_REG_SIZE_U1024	FIELD_PREP(GENMASK_ULL(63, 48), 0x0070)
#define GZVM_REG_SIZE_U2048	FIELD_PREP(GENMASK_ULL(63, 48), 0x0080)

#define GZVM_REG_TYPE_GENERAL2	FIELD_PREP(GENMASK(23, 16), 0x10)

#define GZVM_REG_GENERIC	   0x0000000000000000ULL
#define GZVM_REG_ARM		   0x4000000000000000ULL
#define GZVM_REG_ARM64		   0x6000000000000000ULL

#define GZVM_REG_ARM_CORE		   0x00100000
#define GZVM_REG_ARM_DEMUX		   0x00110000
#define GZVM_REG_ARM_DEMUX_ID_CCSIDR	   0
#define GZVM_REG_ARM64_SYSREG		   0x00130000
#define GZVM_NR_SPSR			   5

#define GZVM_REG_ARM_COPROC_MASK	   0xFFF0000
#define GZVM_REG_ARM_COPROC_SHIFT	   16
#define GZVM_REG_ARM_DEMUX_ID_MASK	   0xFF00
#define GZVM_REG_ARM_DEMUX_ID_SHIFT	   8
#define GZVM_REG_ARM_DEMUX_VAL_MASK	   0xFF
#define GZVM_REG_ARM_DEMUX_VAL_SHIFT	   0
#define GZVM_REG_ARM64_SYSREG_OP0_MASK	   0xC000
#define GZVM_REG_ARM64_SYSREG_OP0_SHIFT	   14
#define GZVM_REG_ARM64_SYSREG_OP1_MASK	   0x3800
#define GZVM_REG_ARM64_SYSREG_OP1_SHIFT	   11
#define GZVM_REG_ARM64_SYSREG_CRN_MASK	   0x0780
#define GZVM_REG_ARM64_SYSREG_CRN_SHIFT	   7
#define GZVM_REG_ARM64_SYSREG_CRM_MASK	   0x0078
#define GZVM_REG_ARM64_SYSREG_CRM_SHIFT	   3
#define GZVM_REG_ARM64_SYSREG_OP2_MASK	   0x0007
#define GZVM_REG_ARM64_SYSREG_OP2_SHIFT	   0

#define GZVM_CREATE_VM             _IO(GZVM_IOC_MAGIC,   0x01)
#define GZVM_CHECK_EXTENSION       _IO(GZVM_IOC_MAGIC,   0x03)

struct gzvm_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size;
};

#define GZVM_SET_MEMORY_REGION     _IOW(GZVM_IOC_MAGIC,  0x40, \
					struct gzvm_memory_region)

#define GZVM_CREATE_VCPU           _IO(GZVM_IOC_MAGIC,   0x41)

#define GZVM_USER_MEM_REGION_GUEST_MEM	(1UL << 0)
#define GZVM_USER_MEM_REGION_PROTECT_FW	(1UL << 1)
#define GZVM_USER_MEM_REGION_STATIC_SWIOTLB	(1UL << 2)

struct gzvm_userspace_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size;
	__u64 userspace_addr;
};

#define GZVM_SET_USER_MEMORY_REGION _IOW(GZVM_IOC_MAGIC, 0x46, \
					 struct gzvm_userspace_memory_region)

#define GZVM_IRQ_VCPU_MASK		0xff
#define GZVM_IRQ_LINE_TYPE		GENMASK(27, 24)
#define GZVM_IRQ_LINE_VCPU		GENMASK(23, 16)
#define GZVM_IRQ_LINE_VCPU2		GENMASK(31, 28)
#define GZVM_IRQ_LINE_NUM		GENMASK(15, 0)

#define GZVM_IRQ_TYPE_CPU		0
#define GZVM_IRQ_TYPE_SPI		1
#define GZVM_IRQ_TYPE_PPI		2

#define GZVM_IRQ_CPU_IRQ		0
#define GZVM_IRQ_CPU_FIQ		1

#define GZVM_IRQ_VCPU2_SHIFT		28
#define GZVM_IRQ_VCPU2_MASK		0xf
#define GZVM_IRQ_TYPE_SHIFT		24
#define GZVM_IRQ_TYPE_MASK		0xf
#define GZVM_IRQ_VCPU_SHIFT		16
#define GZVM_IRQ_VCPU_MASK		0xff
#define GZVM_IRQ_NUM_SHIFT		0
#define GZVM_IRQ_NUM_MASK		0xffff

struct gzvm_irq_level {
	union {
		__u32 irq;
		__s32 status;
	};
	__u32 level;
};

#define GZVM_IRQ_LINE              _IOW(GZVM_IOC_MAGIC,  0x61, \
					struct gzvm_irq_level)

enum gzvm_device_type {
	GZVM_DEV_TYPE_ARM_VGIC_V3_DIST = 0,
	GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST = 1,
	GZVM_DEV_TYPE_MAX,
};

struct gzvm_create_device {
	__u32 dev_type;
	__u32 id;
	__u64 flags;
	__u64 dev_addr;
	__u64 dev_reg_size;
	__u64 attr_addr;
	__u64 attr_size;
};

#define GZVM_CREATE_DEVICE	   _IOWR(GZVM_IOC_MAGIC,  0xe0, \
					struct gzvm_create_device)

#define GZVM_RUN                   _IO(GZVM_IOC_MAGIC,   0x80)

enum {
	GZVM_EXIT_UNKNOWN = 0x92920000,
	GZVM_EXIT_MMIO = 0x92920001,
	GZVM_EXIT_HYPERCALL = 0x92920002,
	GZVM_EXIT_IRQ = 0x92920003,
	GZVM_EXIT_EXCEPTION = 0x92920004,
	GZVM_EXIT_DEBUG = 0x92920005,
	GZVM_EXIT_FAIL_ENTRY = 0x92920006,
	GZVM_EXIT_INTERNAL_ERROR = 0x92920007,
	GZVM_EXIT_SYSTEM_EVENT = 0x92920008,
	GZVM_EXIT_SHUTDOWN = 0x92920009,
	GZVM_EXIT_GZ = 0x9292000a,
	GZVM_EXIT_IDLE = 0x9292000b,
	GZVM_EXIT_IPI = 0x9292000d,
};

enum {
	GZVM_EXCEPTION_UNKNOWN = 0x0,
	GZVM_EXCEPTION_PAGE_FAULT = 0x1,
};

enum {
	GZVM_HVC_PTP = 0x86000001,
	GZVM_HVC_MEM_RELINQUISH = 0xc6000009,
};

struct gzvm_vcpu_run {
	__u32 exit_reason;
	__u8 immediate_exit;
	__u8 padding1[3];
	union {
		struct {
			__u64 phys_addr;
			__u8 data[8];
			__u64 size;
			__u32 reg_nr;
			__u8 is_write;
		} mmio;
		struct {
			__u64 hardware_entry_failure_reason;
			__u32 cpu;
		} fail_entry;
		struct {
			__u32 exception;
			__u32 error_code;
			__u64 fault_gpa;
			__u64 reserved[30];
		} exception;
		struct {
			__u64 args[8];
		} hypercall;
		struct {
			__u32 suberror;
			__u32 ndata;
			__u64 data[16];
		} internal;
		struct {
#define GZVM_SYSTEM_EVENT_SHUTDOWN       1
#define GZVM_SYSTEM_EVENT_RESET          2
#define GZVM_SYSTEM_EVENT_CRASH          3
#define GZVM_SYSTEM_EVENT_WAKEUP         4
#define GZVM_SYSTEM_EVENT_SUSPEND        5
#define GZVM_SYSTEM_EVENT_SEV_TERM       6
#define GZVM_SYSTEM_EVENT_S2IDLE         7
			__u32 type;
			__u32 ndata;
			__u64 data[16];
		} system_event;
		char padding[256];
	};
};

struct gzvm_enable_cap {
	__u64 cap;
	__u64 args[5];
};

#define GZVM_ENABLE_CAP            _IOW(GZVM_IOC_MAGIC,  0xa3, \
					struct gzvm_enable_cap)

struct gzvm_one_reg {
	__u64 id;
	__u64 addr;
};

#define GZVM_GET_ONE_REG	   _IOW(GZVM_IOC_MAGIC,  0xab, \
					struct gzvm_one_reg)
#define GZVM_SET_ONE_REG	   _IOW(GZVM_IOC_MAGIC,  0xac, \
					struct gzvm_one_reg)

#define GZVM_IRQFD_FLAG_DEASSIGN	BIT(0)
#define GZVM_IRQFD_FLAG_RESAMPLE	BIT(1)

struct gzvm_irqfd {
	__u32 fd;
	__u32 gsi;
	__u32 flags;
	__u32 resamplefd;
	__u8  pad[16];
};

#define GZVM_IRQFD	_IOW(GZVM_IOC_MAGIC, 0x76, struct gzvm_irqfd)

enum {
	gzvm_ioeventfd_flag_nr_datamatch = 0,
	gzvm_ioeventfd_flag_nr_pio = 1,
	gzvm_ioeventfd_flag_nr_deassign = 2,
	gzvm_ioeventfd_flag_nr_max,
};

#define GZVM_IOEVENTFD_FLAG_DATAMATCH	(1 << gzvm_ioeventfd_flag_nr_datamatch)
#define GZVM_IOEVENTFD_FLAG_PIO		(1 << gzvm_ioeventfd_flag_nr_pio)
#define GZVM_IOEVENTFD_FLAG_DEASSIGN	(1 << gzvm_ioeventfd_flag_nr_deassign)
#define GZVM_IOEVENTFD_VALID_FLAG_MASK	((1 << gzvm_ioeventfd_flag_nr_max) - 1)

struct gzvm_ioeventfd {
	__u64 datamatch;
	__u64 addr;
	__u32 len;
	__s32 fd;
	__u32 flags;
	__u8  pad[36];
};

#define GZVM_IOEVENTFD	_IOW(GZVM_IOC_MAGIC, 0x79, struct gzvm_ioeventfd)

struct gzvm_dtb_config {
	__u64 dtb_addr;
	__u64 dtb_size;
};

#define GZVM_SET_DTB_CONFIG       _IOW(GZVM_IOC_MAGIC, 0xff, \
				       struct gzvm_dtb_config)

#define GZVM_CREATE_IRQCHIP       _IO(GZVM_IOC_MAGIC,   0x60)

#endif
