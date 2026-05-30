#!/bin/bash
set -e
cd "$(dirname "$0")"
rm -rf build
./configure \
  --target-list=aarch64-softmmu \
  --disable-hvf \
  --disable-nitro \
  --disable-whpx \
  --disable-xen \
  --disable-docs \
  --disable-vde \
  --disable-opengl
make -j$(nproc)
