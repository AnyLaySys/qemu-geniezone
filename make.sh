#!/bin/bash
set -e
cd "$(dirname "$0")"

DEBUG=0
ASAN=0
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --debug) DEBUG=1 ;;
        --asan)  ASAN=1  ;;
        --clean) CLEAN=1 ;;
        *) echo "Usage: $0 [--debug] [--asan] [--clean]"; exit 1 ;;
    esac
    shift
done

rm -rf build

mkdir -p build
cd build

EXTRA=""
if [ "$DEBUG" = 1 ]; then
    EXTRA="$EXTRA --enable-debug"
fi
if [ "$ASAN" = 1 ]; then
    EXTRA="$EXTRA -Dc_args=-fsanitize=address -Dc_link_args=-fsanitize=address"
fi

../configure \
  --target-list=aarch64-softmmu \
  --enable-gzvm \
  --disable-tcg --disable-kvm \
  --disable-hvf --disable-whpx --disable-nitro --disable-mshv --disable-nvmm \
  --disable-xen \
  --disable-opengl --disable-virglrenderer --disable-docs \
  --disable-vde \
  --disable-curl --disable-libiscsi --disable-libnfs --disable-rbd \
  --disable-libssh \
  --disable-bzip2 --disable-lzfse --disable-lzo --disable-snappy \
  --disable-multiprocess \
  --disable-vhost-kernel --disable-vhost-net --disable-vhost-user \
  --disable-vhost-user-blk-server --disable-vhost-vdpa --disable-vhost-crypto \
  $EXTRA

exec make -j$(nproc)
