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

- [x] **1.1 抽 slot 删除逻辑**
  - 已做：`gzvm_remove_overlap_slots()` 提取到 `gzvm-mem.c`，两个 REMOVE 路径合并
  - 预期：~30 行净减少

- [x] **1.2 信号处理器改成 per-VCPU-thread**
  - 已做：`gzvm_install_sigsegv_handler()` 装完 handler 后 block SIGBUS/SIGSEGV 在主线程；vCPU 线程通过 `gzvm_unblock_sigsegv()` 独自放开
  - 效果：只有 vCPU 线程能触发 demand-paging handler，其他子系统不受干扰

- [x] **1.3 SVE/SME 寄存器同步**
  - 已做：通过 ID 寄存器掩码隐藏 SVE/SME feature（`target/arm/gzvm.c`），guest 不暴露就不需要寄存器同步

- [x] **1.4 GIC base 地址不匹配增强诊断**
  - 评估后为 non-issue：virt 机器的 `VIRT_GIC_DIST=0x08000000` `VIRT_GIC_REDIST=0x080A0000` 与内核硬编码值完全一致
  - 现有 `gzvm_set_gic_bases()` 的 warn 永远不会触发，无需增强诊断

---

## 第二批 🟡 功能补齐（中等工作量）

- [x] **2.1 PCIe MSI over GICv2m**
  - 已做：`hw/arm/virt.c` 中 GZVM 选 `VIRT_MSI_CTRL_GICV2M`，移除 GICv2m × GZVM 错误检查
  - 机制：GICv2m 帧 (0x08020000) 不在内核 GIC DIST/REDIST 范围，MMIO 写 → QEMU `arm-gicv2m` → `GZVM_IRQ_LINE` ioctl
  - 待验证：需要在 real HW 上确认 MSI 中断路径通

- [x] **2.2 GZVM_CHECK_EXTENSION fallback**
  - 已做：`target/arm/gzvm.c` 中 `GZVM_CAP_ARM_VM_IPA_SIZE` ioctl 失败时默认 40-bit (PARANGE=2)
  - 旧内核会 sanitize PARANGE 到 32-bit，不 fallback 的话 >4GB 内存的 guest 会崩

- [x] **2.3 把 GZVM virt 逻辑拆出 `virt.c`**
  - 已做：新建 `hw/arm/virt-gzvm.c` + `include/hw/arm/virt-gzvm.h`
  - 抽出 6 个函数：ram_base、GIC base、PSCI conduit、DTB 地址、ITS 阻塞、整体 init
  - `virt.c` 中 6 个散点改为函数调用，9 个散点因流程约束保留原位

- [x] **2.4 支持 secondary CPU 启动不依赖内核 PSCI**
  - 评估后为 non-issue：GenieZone 驱动在 EL2 处理 PSCI，不需要 QEMU fallback
  - 已加 `GZVM_EXIT_HYPERCALL` 的 warn_report，未识别的 hypercall 会打日志

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
