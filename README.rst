================================
QEMU - GenieZone Hypervisor (GZVM)
================================

This is a fork of QEMU with support for MediaTek's **GenieZone (GZVM)**
type-1 hypervisor. GZVM is a pure-EL2 hypervisor for ARM, used in
MediaTek chipsets (Dimensity, Kompanio, etc.). It runs stand-alone in
EL2 and doesn't depend on any particular host VM.

This fork adds a ``gzvm`` accelerator (``-accel gzvm``) so you can run
QEMU on top of GenieZone, plus a ``virt-gzvm`` machine type for ARM
guests.

U-Boot is also supported as a guest — see the dedicated port at
`Alhkxsj/u-boot <https://github.com/Alhkxsj/u-boot>`_.

Building
========

QEMU is multi-platform software intended to be buildable on all modern
Linux platforms, OS-X, Win32 (via the Mingw64 toolchain) and a variety
of other UNIX targets. The simple steps to build QEMU are:

.. code-block:: shell

  cd qemu-geniezone
  chmod +x make.sh
  ./make.sh

You'll need a cross-compiler toolchain for the target architecture if
you're building for something other than your host.

Additional information can also be found online via the QEMU website:

* `<https://wiki.qemu.org/Hosts/Linux>`_
* `<https://wiki.qemu.org/Hosts/Mac>`_
* `<https://wiki.qemu.org/Hosts/W32>`_


Documentation
=============

Upstream QEMU docs can be found at
`<https://www.qemu.org/documentation/>`_. The GZVM-specific code lives
under ``target/arm/gzvm/`` and ``hw/arm/virt-gzvm.c``.


Why this exists
===============

The upstream GenieZone kernel driver provides the ``/dev/gzvm``
interface for VMMs, but there wasn't a ready-to-use QEMU backend.
This fork fills that gap.

Related projects:

* `qemu-gunyah <https://github.com/AnyLaySys/qemu-gunyah>`_ -- same
  idea for Qualcomm's Gunyah hypervisor
* `als <https://github.com/AnyLaySys/als>`_ -- AnyLaySys, the umbrella
  project these all live under


History
=======

For the full list of changes from upstream QEMU, look at the git log.
This repo tracks the QEMU stable branch and cherry-picks or reworks
commits on top.

License
=======

QEMU as a whole is released under the GNU General Public License,
version 2. For full licensing details, consult the LICENSE file.
