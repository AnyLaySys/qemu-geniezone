# GZVM QEMU — Agent Context

## 项目状态

QEMU fork + GenieZone (GZVM) hypervisor 加速器支持。核心功能已跑通（U-Boot/Linux 能启动），细节见 [ROADMAP.md](ROADMAP.md)。

## CodeGraph 知识图谱

已安装并配置好 [CodeGraph](https://github.com/colbymchenry/codegraph)（v0.9.9）作为 MCP server。

**索引状态：**
- 文件：6,645（其中 C 代码 5,968 个文件）
- 符号节点：131,692
- 边（调用/引用关系）：351,186
- 数据库：206 MB SQLite（本地，100% 离线）

**配置位置：** `opencode.jsonc` — MCP server 已注册，agent 启动时自动加载。

### AI 使用 CodeGraph 的方式

CodeGraph 提供了 8 个 MCP 工具，按使用频率排列：

| 工具 | 用途 | 何时用 |
|---|---|---|
| `codegraph_explore` | **首选工具。** 一次调用返回相关符号的源码 + 关系图 | 理解任何功能、流程、架构 |
| `codegraph_search` | 按名字找符号位置 | 知道名字但不知道在哪个文件 |
| `codegraph_callers` | 找谁调用了这个函数 | 改函数前看影响范围 |
| `codegraph_callees` | 找这个函数调用了什么 | 追踪调用链路 |
| `codegraph_impact` | 分析改一个符号会影响哪些代码 | 重构前必查 |
| `codegraph_node` | 获取单个符号的完整源码 + 所有重载 | explore 返回内容被截断时 |
| `codegraph_files` | 文件树（比 ls/Glob 快） | 看目录结构 |
| `codegraph_status` | 检查索引健康度、是否有待同步文件 | 怀疑索引过期时 |

**关键原则：** 先用 `codegraph_explore` 而不是 Read/Grep/Glob。知识图谱已经预建好，直接查比搜索文件快得多。

### 自动同步

- 文件修改后 ~2s 内自动同步到索引
- MCP server 重连时自动 catch-up
- 修改后未同步的文件会在工具响应里加 `⚠️` 标记

---

## 其他 MCP 工具

除了 CodeGraph，还配置了以下 MCP server，各有侧重：

| 工具 | 版本 | 核心用途 | 适用场景 |
|---|---|---|---|
| **opencode-codebase-index** | 0.10.0 | **语义搜索** — 按意义找代码 | "找用户认证逻辑" 即使函数叫 `check_creds` |
| **codesight** | 1.14.0 | **爆裂半径 + 项目上下文** — BFS 遍历 import 图 | 改文件前看影响范围、项目结构概览 |
| **aptu-coder** | 0.10.0 | **轻量 AST 分析** — 目录/文件/符号结构 + 调用图 | 快速浏览目录结构、查文件符号、看调用图 |

### 使用方式

```
找代码但不知道函数名 → opencode-codebase-index 的语义搜索
改代码前看影响范围   → codesight 或 codegraph_impact
找死代码/无用导出     → codebase-intelligence hotspots
理解模块架构          → codebase-intelligence overview
快速看目录/文件结构   → aptu-coder analyze_directory / analyze_file
看函数调用图          → aptu-coder analyze_symbol
```

配置在 `opencode.jsonc`，agent 启动时自动加载。

---

## 开发导航

### GZVM 关键目录和文件

| 路径 | 作用 |
|---|---|
| `accel/gzvm/gzvm.c` | vCPU 创建 + 执行主循环 |
| `accel/gzvm/gzvm-accel-ops.c` | `-accel gzvm` 注册入口 |
| `accel/gzvm/gzvm-ioctl.c` | 与 `/dev/gzvm` 驱动的 ioctl 通信 |
| `accel/gzvm/gzvm-mem.c` | 内存槽管理（二分查找、添加/删除） |
| `accel/gzvm/gzvm-mmio.c` | MMIO 退出处理 |
| `accel/gzvm/gzvm-signal.c` | SIGBUS/SIGSEGV handler 按需分页 |
| `accel/gzvm/gzvm-irq.c` | irqfd / ioeventfd 管理 |
| `accel/gzvm/gzvm-vcpu.c` | vCPU IPI 信号 |
| `accel/gzvm/gzvm-vm-start.c` | 受保护 VM 启动序列 |
| `target/arm/gzvm.c` | ARM 寄存器同步、CPU feature 探测 |
| `hw/intc/arm_gicv3_gzvm.c` | GZVM GICv3 后端 |
| `hw/arm/virt.c` | 15 个 `gzvm_enabled()` 集成点 |

### 构建

```bash
./make.sh              # debug 构建
./make.sh --release    # release
./make.sh --clean      # 重新构建
```

只构建 `aarch64-softmmu` 目标。

### 编码规则

- 不改 upstream QEMU 通用代码，所有 GZVM 逻辑用 `if (gzvm_enabled())` 保护
- 代码风格遵循 QEMU Coding Style
- 所有改动必须 `./make.sh` 编译通过

---

## 优化路线图（按优先级）

### 短期 — 低风险快速修复

1. **抽 slot 删除逻辑** (`accel/gzvm/gzvm-mem.c`) — 重复代码，~30 行净减少
2. **信号处理器 per-VCPU-thread** (`accel/gzvm/gzvm-signal.c`) — 全局 SIGBUS handler 会抢其他子系统信号
3. **SVE/SME 寄存器同步** (`target/arm/gzvm.c`) — host 有 SVE 时 guest 用 SVE 指令会挂

### 中期 — 功能补齐

4. **Dirty page tracking** — migration 的 blocker，需要内核先支持
5. **In-kernel GIC** — 等内核支持 VGIC 设备后切换
6. **PCIe MSI over GICv2m** — 验证 GICv2m 路径

### 长期 — 架构改进

7. **Guest debug / gdbstub**
8. **把 `virt_gzvm_init()` 抽出 `virt.c`**
9. **PMU 虚拟化**
