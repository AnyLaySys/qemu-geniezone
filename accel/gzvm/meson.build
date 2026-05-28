gzvm_ss = ss.source_set()
gzvm_ss.add(files(
  'gzvm.c',
  'gzvm-accel-ops.c',
))
specific_ss.add_all(when: 'CONFIG_GZVM', if_true: gzvm_ss)
