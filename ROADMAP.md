# 🗺️ GenieZone (GZVM) QEMU — 优化路线图

> **其他 AI 注意：** 这是项目的全局优化方向。所有改动的最终目标是让 GZVM 在 QEMU 里的集成达到 KVM 同等级别的完整性，同时不影响 upstream QEMU 的正常功能。

---

## 📊 当前状态总览

```
GZVM Accelerator  ████████░░  80%   (核心跑通，U-Boot/Linux 能启动)
ARM CPU 集成      ██████░░░░  60%   (寄存器同步 ok，SVE/SME 缺少)
GICv3             ██████░░░░  60%   (emulated GIC 能用，in-kernel 没有)
内存管理          ██████░░░░  60%   (slot 管理能用，代码有重复)
启动流程          ████████░░  80%   (FDT + primary/secondary CPU ok)
PCIe/MSI          ██░░░░░░░░  20%   (ITS 直接 exit，只能用 GICv2m)
Guest Debug       ██░░░░░░░░  20%   (基本 skeleton，gdbstub 没通)
Migration         ░░░░░░░░░░   0%   (加了 blocker，内核缺 dirty log)
ACPI              ░░░░░░░░░░   0%   (只有 FDT)
信号处理          ██████░░░░  60%   (SIGSEGV/SIGBUS 有，但是全局的)
```

---

## 第一批 🔵 快速修复（低风险，高收益）

这些改动范围小，已经有足够调试经验，可直接动手。

- [ ] **1.1 抽 slot 删除逻辑**
  - 位置：`accel/gzvm/gzvm-mem.c`
  - `gzvm_set_phys_mem()` 里删除 overlap slot 的代码出现了两次（add=false 路径和 add=true 路径），一模一样
  - 动作：抽成 `static void gzvm_remove_overlap_slot_locked(GZVMState *s, hwaddr addr, uint64_t size)`
  - 预期：~30 行净减少

- [ ] **1.2 信号处理器改成 per-VCPU-thread**
  - 位置：`accel/gzvm/gzvm-signal.c`
  - 现在 `gzvm_install_sigsegv_handler()` 装的是全局 handler，任何线程的 SIGSEGV/SIGBUS 都会触发 demand paging
  - 改法：只在 VCPU 线程初始化时装（`gzvm_init_vcpu` 或 `gzvm_cpu_thread_fn`），用 `pthread_sigmask` 限制作用域
  - 风险：QEMU 其他子系统（比如某些 block 驱动）也可能依赖 SIGBUS，全局 handler 会抢

- [ ] **1.3 SVE/SME 寄存器同步**
  - 位置：`target/arm/gzvm.c` 的 `gzvm_arch_put_registers()`
  - 现在 probe 了 SVE/SME feature 但没同步寄存器。如果 host CPU 支持 SVE，guest 里用 SVE 指令会出问题
  - 参考：KVM 的 `kvm_arch_put_sve_regs()` 在 `target/arm/kvm.c`
  - 注意：GZVM_GET_ONE_REG 内核不支持，所以存不了状态，至少 SET_ONE_REG 要做

- [ ] **1.4 GIC base 地址不匹配增强诊断**
  - 位置：`target/arm/gzvm.c:gzvm_set_gic_bases()`
  - 内核驱动硬编码 GIC DIST=0x08000000 REDIST=0x080A0000 和 virt 机器的地址完全不匹配
  - 现在只是 warn。改成：把内存映射打印出来 + 在 device tree 里检查地址是否正确
  - 长远：跟内核 side 对齐地址，或者在 QEMU 侧用 MemoryRegion alias 做重映射

---

## 第二批 🟡 功能补齐（中等工作量）

- [ ] **2.1 PCIe MSI over GICv2m**
  - 位置：`hw/arm/virt.c`
  - 现在 `gzvm_enabled()` 时 `msi_controller` 选了 `GICV2M`，但这个路径没充分测试
  - 动作：验证 GICv2m MSI 写入 GIC 寄存器是否能通过 MMIO trap 到达 `arm_gicv3_gzvm.c`
  - 如果通：加个 auto-test 确认

- [ ] **2.2 GZVM_CHECK_EXTENSION fallback**
  - 位置：`target/arm/gzvm.c:gzvm_arm_set_cpu_features_from_host()`
  - `GZVM_CAP_ARM_VM_IPA_SIZE` 的探测依赖 ioctl，旧内核可能不支持
  - 动作：如果 ioctl 返回不支持，fallback 到固定值（比如 40-bit）

- [ ] **2.3 把 GZVM virt 逻辑拆出 `virt.c`**
  - 位置：新建 `hw/arm/virt-gzvm.c`
  - 现在 GZVM 的集成是 15 个散点 `if (gzvm_enabled())` 散布在整个 `virt.c` 里
  - 动作：仿照 KVM 的 `virt_kvm_init()`，抽一个 `virt_gzvm_init()`，把以下逻辑聚到一起：
    - ram_base 设置
    - GIC base 设置
    - PSCI conduit 选择
    - DTB 地址传递
    - ITS 阻塞
  - `virt.c` 里只剩下 `gzvm_enabled() ? virt_gzvm_init(vms) : /* default */`

- [ ] **2.4 支持 secondary CPU 启动不依赖内核 PSCI**
  - 位置：`hw/arm/boot.c` + `accel/gzvm/gzvm.c`
  - 现在如果内核不支持 PSCI in-kernel，secondary CPU 起不来
  - 动作：通过 `GZVM_CHECK_EXTENSION` 查内核是否支持 PSCI，不支持的 fallback 到 QEMU 软件 PSCI（类似 KVM 的 `kvm_psci`）

---

## 第三批 🔴 架构改进（大工作量）

- [ ] **3.1 In-kernel GIC（如果内核能力到位）**
  - 现在 GZVM 用 QEMU emulated GICv3 + `GZVM_IRQ_LINE` ioctl 注入中断，性能差
  - 如果内核驱动支持 VGIC 设备创建（类似 KVM 的 `KVM_DEVICE_ARM_VGIC_V3`），`arm_gicv3_gzvm.c` 可以简化成薄包装

- [ ] **3.2 Dirty page tracking**
  - 内核需要支持 `GZVM_GET_DIRTY_LOG` 或类似 API
  - QEMU 侧参考 KVM 的 `kvm_physical_sync_dirty_bitmap()` + `kvm_log_start/stop`
  - 有 dirty log 之后才能去掉 migration blocker

- [ ] **3.3 Guest debug / gdbstub**
  - 内核需要支持 `GZVM_SET_GUEST_DEBUG` 或硬件断点寄存器设置
  - QEMU 侧参考 KVM 的 `kvm_arch_update_guest_debug()` + `kvm_arch_insert_sw_breakpoint()`

- [ ] **3.4 PMU 虚拟化**
  - 内核需要支持 PMU 事件虚拟化
  - QEMU 侧在设备树里加 PMU node + 处理 PMU 中断

---

## 📐 架构决策记录

### 为什么不直接写 `virt-gzvm` 机器类型？

现有 GZVM 集成选择在 `virt.c` 里加 `if (gzvm_enabled())`，而不是独立机器类型。理由是：

- 跟 KVM 的策略一致（`-M virt` + `-accel kvm` vs `-accel gzvm`）
- 减少 `-M` 参数的组合爆炸
- 但缺点是 GZVM-specific 逻辑散布在 `virt.c` 各处

**结论：** 先抽 `virt_gzvm_init()` 函数，不独立成机器类型。除非 GZVM 和 virt 的分歧大到共享代码成本高于独立维护成本。

### Slot 管理策略

当前用 `sorted_ids` 数组做二分查找 + O(n) 插入/删除。512 最大 slot 的情况下性能够用，不改。如果未来 slot 数量大幅增加，可以换成 interval tree 或 rbtree。

---

## 📋 给其他 AI 的导航

如果你是一个 AI agent 扫描这个项目：

1. **ROADMAP.md** ← 你正在看。这里是全局计划。
2. `accel/gzvm/` — GZVM 加速器核心代码
3. `target/arm/gzvm.c` — ARM 特定 GZVM 代码（寄存器同步、CPU feature）
4. `hw/intc/arm_gicv3_gzvm.c` — GZVM 专用 GICv3
5. `hw/arm/virt.c` — 搜索 `gzvm_enabled()` 找到集成点
6. `hw/arm/boot.c` — GZVM 启动流程集成
7. `.opencode/rules.md` — 操作规则（如果有）

### 技术约束

- 不支持 `GZVM_GET_ONE_REG`（读寄存器只能 fallback）
- 不支持 dirty page tracking
- ITS 不支持，MSI 走 GICv2m
- GIC DIST/REDIST 地址被内核硬编码
- GZVM 只跑在 `aarch64-softmmu`
