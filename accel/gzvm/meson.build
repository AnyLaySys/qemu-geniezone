gzvm_ss = ss.source_set()
gzvm_ss.add(files(
  'gzvm.c',
  'gzvm-accel-ops.c',
  'gzvm-ioctl.c',
  'gzvm-irq.c',
  'gzvm-mem.c',
  'gzvm-mmio.c',
  'gzvm-signal.c',
  'gzvm-vcpu.c',
  'gzvm-vm-start.c',
))
specific_ss.add_all(when: 'CONFIG_GZVM', if_true: gzvm_ss)
