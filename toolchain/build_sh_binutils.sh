#!/bin/bash
# Build GNU binutils targeting sh-elf (SH-2 / SuperH) for cross-disassembly + assembly.
set -euo pipefail
VER=2.44
ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$ROOT/sh-elf"
cd "$ROOT/build"
[ -f "binutils-$VER.tar.xz" ] || curl -fL -O "https://ftp.gnu.org/gnu/binutils/binutils-$VER.tar.xz"
[ -d "binutils-$VER" ] || tar xf "binutils-$VER.tar.xz"
rm -rf b-sh && mkdir b-sh && cd b-sh
../binutils-$VER/configure --target=sh-elf --prefix="$PREFIX" \
  --disable-nls --disable-werror --disable-gdb --disable-sim --disable-libdecnumber --disable-readline \
  --with-system-zlib
# hw.ncpu is macOS's spelling and the only one this had; _NPROCESSORS_ONLN is
# answered by macOS, Linux and MSYS2 alike, so the script builds under all three.
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install
echo "OK: $PREFIX/bin"
