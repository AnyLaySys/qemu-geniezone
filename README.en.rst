================================
QEMU - GenieZone Hypervisor (GZVM)
================================

This project adds AArch64 virtualization support for the GenieZone
Hypervisor to QEMU. In this tree, the host interface for GenieZone is
called GZVM. QEMU reaches it through the new ``gzvm`` accelerator and
the host kernel's ``/dev/gzvm`` device, then runs guest VMs on top of
GenieZone.

The goal is straightforward: make QEMU's Arm ``virt`` machine work well
with GZVM. This README covers the GZVM integration only.


Highlights
==========

The GZVM integration covers the main paths needed to boot and run an
Arm guest:

* select the accelerator with ``-accel gzvm``;
* create VMs and vCPUs through the GZVM ioctl interface;
* register guest RAM through GZVM memory slots;
* initialize AArch64 CPU registers and probe host CPU features for
  ``-cpu host``;
* integrate with QEMU's Arm ``virt`` machine, including DTB handoff,
  RAM base setup, GIC base setup, and PSCI-related boot flow;
* handle MMIO and guest exceptions after ``GZVM_RUN`` returns;
* wire IRQFD and IOEVENTFD for a more direct device notification path;
* provide GICv3-related support in ``hw/intc/arm_gicv3_gzvm.c``;
* support protected-VM configuration with ``-accel gzvm,protected=on``
  when the host kernel exposes the required GZVM capability.


Source layout
=============

GZVM-specific code is mainly in:

* ``accel/gzvm/``;
* ``target/arm/gzvm.c`` and ``target/arm/gzvm_arm.h``;
* ``include/system/gzvm.h`` and ``include/system/gzvm_int.h``;
* ``hw/intc/arm_gicv3_gzvm.c``;
* GZVM integration points in ``hw/arm/virt.c`` and ``hw/arm/boot.c``.


Platform and build
==================

The target is ``aarch64-softmmu``. Meson enables ``CONFIG_GZVM`` on
Linux or Android hosts when the ``gzvm`` feature is available. At
runtime, the host system must provide an accessible ``/dev/gzvm`` device
from a compatible GZVM kernel driver.

Use the helper script to configure and build:

.. code-block:: shell

  ./make.sh

Useful options:

.. code-block:: shell

  ./make.sh --debug
  ./make.sh --asan
  ./make.sh --clean

The resulting binary is normally:

.. code-block:: shell

  build/qemu-system-aarch64


Run example
===========

Use the regular Arm ``virt`` machine and enable GZVM with
``-accel gzvm``. This example is kept on one line so it can be copied
directly:

.. code-block:: shell

  ./qemu-system-aarch64 -M virt -accel gzvm -cpu host -m 4G -kernel ./kernel -append "root=/dev/vda2 rw console=ttyAMA0" -drive if=none,id=hd,file=./disk.img,format=raw -device virtio-blk-pci,drive=hd -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -device virtio-gpu-pci -nographic

For protected VM mode, use:

.. code-block:: shell

  -accel gzvm,protected=on

This option depends on the matching GZVM capability in the host kernel.


Documentation
=============

For general QEMU usage, see the upstream documentation:

* `<https://www.qemu.org/documentation/>`_

GZVM engineering notes and follow-up work are tracked in ``ROADMAP.md``.


License
========

QEMU is released under the GNU General Public License, version 2. See
``LICENSE`` and the file headers for details.
