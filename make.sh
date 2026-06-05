#!/bin/bash
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build
if [ ! -f meson-info/intro-buildoptions.json ]; then
    ../configure \
      --target-list=aarch64-softmmu \
      --disable-hvf --disable-nitro --disable-whpx \
      --disable-xen --disable-docs --disable-vde --disable-opengl
fi
exec make -j$(nproc)
