================================
QEMU - GenieZone Hypervisor (GZVM)
================================

本项目为 QEMU 增加面向 GenieZone Hypervisor 的 AArch64 虚拟化支持。
在这份代码里，GenieZone 的宿主接口称为 GZVM；QEMU 通过新增的
``gzvm`` accelerator 访问宿主内核暴露的 ``/dev/gzvm``，从而把 guest VM
运行在 GenieZone 之上。

项目目标很明确：让 Arm/AArch64 的 ``virt`` 机器可以直接使用 GZVM。
这份 README 只介绍 GZVM 相关内容。


能力概览
========

GZVM 集成已经覆盖 QEMU 启动 guest 所需的主要路径：

* 通过 ``-accel gzvm`` 选择 GZVM accelerator；
* 通过 GZVM ioctl 创建 VM 和 vCPU；
* 使用 GZVM memory slot 注册 guest RAM；
* 支持 AArch64 CPU 寄存器初始化，并为 ``-cpu host`` 探测宿主 CPU feature；
* 接入 QEMU Arm ``virt`` 机器，处理 DTB 传递、RAM base、GIC base 和
  PSCI 相关启动流程；
* 处理 ``GZVM_RUN`` 返回后的 MMIO 和 guest exception；
* 接入 IRQFD 和 IOEVENTFD，让设备通知路径更直接；
* 提供 GICv3 相关支持，代码位于 ``hw/intc/arm_gicv3_gzvm.c``；
* 支持 protected VM 配置；宿主内核支持对应 capability 时，可使用
  ``-accel gzvm,protected=on`` 启用。


代码位置
========

GZVM 相关代码主要在这些位置：

* ``accel/gzvm/``；
* ``target/arm/gzvm.c`` 和 ``target/arm/gzvm_arm.h``；
* ``include/system/gzvm.h`` 和 ``include/system/gzvm_int.h``；
* ``hw/intc/arm_gicv3_gzvm.c``；
* ``hw/arm/virt.c`` 和 ``hw/arm/boot.c`` 中的 GZVM 集成点。


平台与构建
==========

当前目标是 ``aarch64-softmmu``。Meson 在 Linux 或 Android 宿主上、且
``gzvm`` feature 可用时启用 ``CONFIG_GZVM``。运行时需要宿主系统提供
可访问的 ``/dev/gzvm``，并且该设备来自兼容的 GZVM 内核驱动。

仓库提供了一个构建脚本：

.. code-block:: shell

  ./make.sh

常用选项：

.. code-block:: shell

  ./make.sh --debug
  ./make.sh --asan
  ./make.sh --clean

构建产物通常位于：

.. code-block:: shell

  build/qemu-system-aarch64


运行示例
========

GZVM 使用普通 Arm ``virt`` 机器，并通过 ``-accel gzvm`` 启用：

.. code-block:: shell

  ./build/qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu host \
    -accel gzvm \
    -m 1024 \
    -smp 4 \
    -nographic \
    -kernel Image \
    -append "console=ttyAMA0" \
    -initrd rootfs.cpio

protected VM 可使用：

.. code-block:: shell

  -accel gzvm,protected=on

该选项依赖宿主内核支持相应的 GZVM capability。


文档
====

通用 QEMU 用法请参考上游文档：

* `<https://www.qemu.org/documentation/>`_

GZVM 工程状态和后续工作记录在 ``ROADMAP.md``。


许可证
======

QEMU 使用 GNU General Public License version 2 发布。详细信息见
``LICENSE`` 和各源码文件头部。
