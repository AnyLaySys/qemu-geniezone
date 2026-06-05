#!/bin/bash
set -e
cd "$(dirname "$0")"

DEBUG=0
ASAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --debug) DEBUG=1 ;;
        --asan)  ASAN=1  ;;
        *) echo "Usage: $0 [--debug] [--asan]"; exit 1 ;;
    esac
    shift
done

mkdir -p build
cd build

EXTRA=""
if [ "$DEBUG" = 1 ]; then
    EXTRA="$EXTRA --enable-debug"
fi
if [ "$ASAN" = 1 ]; then
    EXTRA="$EXTRA -Dc_args=-fsanitize=address -Dc_link_args=-fsanitize=address"
fi

if [ ! -f meson-info/intro-buildoptions.json ]; then
    ../configure \
      --target-list=aarch64-softmmu \
      --disable-hvf --disable-nitro --disable-whpx \
      --disable-xen --disable-docs --disable-vde --disable-opengl \
      $EXTRA
fi
exec make -j$(nproc)
