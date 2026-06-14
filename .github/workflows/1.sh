#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
sudo apt install -y python3 python3-venv python3-pip cmake ninja-build meson pkg-config git curl
NdkPath="$HOME/android-ndk-r30-beta1"
ApiLevel="36"
NCpu="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
BuildDir="$(pwd)/build"
OutDir="$BuildDir/out"
Prefix="$BuildDir/sysroot"
SrcDir="$BuildDir/src"
LibffiVer="3.4.4"
Pcre2Ver="10.44"
GlibVer="2.83.0"
PixmanVer="0.42.2"
LibusbVer="1.0.27"
BuildPixman="1"
# virglrenderer and libepoxy not needed (--disable-opengl --disable-virglrenderer)
HostOS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$HostOS" in
  linux)   HostTag="linux-x86_64" ;;
  darwin)  HostTag="darwin-x86_64" ;;
  *) echo "不支持此系统: $HostOS" >&2; exit 1 ;;
esac
Toolchain="$NdkPath/toolchains/llvm/prebuilt/$HostTag"
TargetTriple=aarch64-linux-android
MesonCpu=aarch64
CmakeAbi="arm64-v8a"
export CC="$Toolchain/bin/${TargetTriple}${ApiLevel}-clang"
export CXX="$Toolchain/bin/${TargetTriple}${ApiLevel}-clang++"
export AR="$Toolchain/bin/llvm-ar"
export NM="$Toolchain/bin/llvm-nm"
export STRIP="$Toolchain/bin/llvm-strip"
export RANLIB="$Toolchain/bin/llvm-ranlib"
export LD="$Toolchain/bin/ld"
export OBJCOPY="$Toolchain/bin/llvm-objcopy"
export PKG_CONFIG_PATH="$Prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$Prefix/lib/pkgconfig"
export CFLAGS="-fPIC -fPIE -ftls-model=global-dynamic"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-pie"
mkdir -p "$Prefix" "$SrcDir" "$OutDir"
fetch() {
  local url="$1" out="$2"
  if [ ! -f "$out" ]; then
    echo "下载 $url"
    curl -L --fail -o "$out" "$url"
  fi
}
cd "$SrcDir"
fetch "https://github.com/libffi/libffi/releases/download/v${LibffiVer}/libffi-${LibffiVer}.tar.gz" "libffi-${LibffiVer}.tar.gz"
[ -d "libffi-${LibffiVer}" ] || tar xf "libffi-${LibffiVer}.tar.gz"
mkdir -p "$OutDir/libffi"
cd "$OutDir/libffi"
if [ -f Makefile ]; then make distclean || true; fi
echo "配置 libffi ${LibffiVer}"
"$SrcDir/libffi-${LibffiVer}/configure" \
  --host="${TargetTriple}" \
  --prefix="$Prefix" \
  --enable-shared \
  --disable-static \
  --disable-exec-static-tramp
echo "编译 libffi"
make -j"$NCpu"
make install
cd "$SrcDir"
fetch "https://github.com/PhilipHazel/pcre2/releases/download/pcre2-${Pcre2Ver}/pcre2-${Pcre2Ver}.tar.bz2" "pcre2-${Pcre2Ver}.tar.bz2"
[ -d "pcre2-${Pcre2Ver}" ] || tar xf "pcre2-${Pcre2Ver}.tar.bz2"
mkdir -p "$OutDir/pcre2"
cd "$OutDir/pcre2"
echo "配置 PCRE2 ${Pcre2Ver}"
cmake -G Ninja "$SrcDir/pcre2-${Pcre2Ver}" \
  -DCMAKE_TOOLCHAIN_FILE="$NdkPath/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$CmakeAbi" \
  -DANDROID_PLATFORM="android-${ApiLevel}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$Prefix" \
  -DCMAKE_C_FLAGS="-ftls-model=global-dynamic" \
  -DBUILD_SHARED_LIBS=ON \
  -DPCRE2_BUILD_PCRE2_8=ON \
  -DPCRE2_BUILD_PCRE2_16=OFF \
  -DPCRE2_BUILD_PCRE2_32=OFF \
  -DPCRE2_SUPPORT_JIT=OFF
echo "编译 PCRE2"
cmake --build . -j"$NCpu"
cmake --install .
cd "$SrcDir"
fetch "https://download.gnome.org/sources/glib/${GlibVer%.*}/glib-${GlibVer}.tar.xz" "glib-${GlibVer}.tar.xz"
[ -d "glib-${GlibVer}" ] || tar xf "glib-${GlibVer}.tar.xz"
perl -0pi -e "s/\\n  'lchmod',//" "glib-${GlibVer}/meson.build"
perl -0pi -e "s/if cc\\.has_header_symbol\\('pthread\\.h', 'pthread_getaffinity_np', prefix : pthread_prefix\\)/if false and cc.has_header_symbol('pthread.h', 'pthread_getaffinity_np', prefix : pthread_prefix)/" "glib-${GlibVer}/meson.build"
MesonCross="$OutDir/glib.cross"
cat > "$MesonCross" <<EOF
[binaries]
c = '${CC}'
cpp = '${CXX}'
ar = '${AR}'
strip = '${STRIP}'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-fPIC','-fPIE','-ftls-model=global-dynamic']
c_link_args = ['-pie']
[host_machine]
system = 'linux'
cpu_family = '${MesonCpu}'
cpu = '${MesonCpu}'
endian = 'little'
EOF
mkdir -p "$OutDir/glib"
cd "$OutDir/glib"
[ -f build.ninja ] && rm -rf *
echo "配置 GLib ${GlibVer}"
meson setup . "$SrcDir/glib-${GlibVer}" \
  --cross-file "$MesonCross" \
  --prefix "$Prefix" \
  -Ddefault_library=shared \
  -Doptimization=2 \
  -Ddebug=false \
  -Dglib_debug=disabled \
  -Dtests=false \
  -Dman-pages=disabled \
  -Ddocumentation=false \
  -Dselinux=disabled \
  -Dlibmount=disabled \
  -Dnls=disabled
echo "编译 GLib"
meson compile -j"$NCpu"
meson install
if [ "$BuildPixman" = "1" ]; then
  cd "$SrcDir"
  fetch "https://www.cairographics.org/releases/pixman-${PixmanVer}.tar.gz" "pixman-${PixmanVer}.tar.gz"
  [ -d "pixman-${PixmanVer}" ] || tar xf "pixman-${PixmanVer}.tar.gz"
  mkdir -p "$OutDir/pixman"
  cd "$OutDir/pixman"
  if [ -f Makefile ]; then make distclean || true; fi
  echo "配置 pixman ${PixmanVer}"
  "$SrcDir/pixman-${PixmanVer}/configure" \
    --host="${TargetTriple}" \
    --prefix="$Prefix" \
    --disable-static \
    --disable-arm-a64-neon
  echo "编译 pixman"
  make -j"$NCpu"
  make install
fi
cd "$SrcDir"
fetch "https://github.com/libusb/libusb/releases/download/v${LibusbVer}/libusb-${LibusbVer}.tar.bz2" "libusb-${LibusbVer}.tar.bz2"
[ -d "libusb-${LibusbVer}" ] || tar xf "libusb-${LibusbVer}.tar.bz2"
mkdir -p "$OutDir/libusb"
cd "$OutDir/libusb"
if [ -f Makefile ]; then make distclean || true; fi
echo "配置 libusb ${LibusbVer}"
"$SrcDir/libusb-${LibusbVer}/configure" \
  --host="${TargetTriple}" \
  --prefix="$Prefix" \
  --enable-shared \
  --disable-static \
  --disable-udev
echo "编译 libusb"
make -j"$NCpu"
make install
# libepoxy and virglrenderer skipped (--disable-opengl --disable-virglrenderer)
