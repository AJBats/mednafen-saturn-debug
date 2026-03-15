#!/bin/bash
# Build Mednafen with GCC 4.9.4 cross-compiler
set -e

export PATH="/opt/gcc-4.9.4-mingw64/bin:$PATH"
GCC494_SYSROOT=/opt/gcc-4.9.4-mingw64/x86_64-w64-mingw32

echo "=== Compiler version ==="
x86_64-w64-mingw32-gcc --version | head -1
x86_64-w64-mingw32-g++ --version | head -1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Create include symlink (git doesn't preserve symlinks on Windows)
# ln -sfn fails on NTFS directory symlinks ("Is a directory"), so skip if correct
if [ ! -L "$SCRIPT_DIR/include/mednafen" ]; then
    ln -sn "$SCRIPT_DIR/src" "$SCRIPT_DIR/include/mednafen"
fi

# Clean previous build
cd src
make clean 2>/dev/null || true
cd ..

# Copy SDL2/FLAC/zlib/iconv static libs into our sysroot so GCC 4.9.4 finds them
# (these were built with GCC 13 but are C libs — ABI-compatible)
for lib in libSDL2.a libSDL2main.a libz.a libFLAC.a libiconv.a; do
  cp -n /usr/x86_64-w64-mingw32/lib/$lib $GCC494_SYSROOT/lib/ 2>/dev/null || true
done
# Copy SDL2 headers
cp -rn /usr/x86_64-w64-mingw32/include/SDL2 $GCC494_SYSROOT/include/ 2>/dev/null || true

# GCC 4.9.4's libmsvcrt.a is missing _wassert and __acrt_iob_func.
# Replace with system GCC 13's libmsvcrt.a which has them.
cp -f /usr/x86_64-w64-mingw32/lib/libmsvcrt.a $GCC494_SYSROOT/lib/

# Configure with GCC 4.9.4 — ONLY our sysroot in library paths
./configure \
  --host=x86_64-w64-mingw32 \
  --disable-nls \
  --disable-sdltest \
  --with-sdl-prefix=/opt/gcc-4.9.4-mingw64 \
  CFLAGS="-O2 -I$GCC494_SYSROOT/include/SDL2 -DFLAC__NO_DLL" \
  CXXFLAGS="-O2 -I$GCC494_SYSROOT/include/SDL2 -DFLAC__NO_DLL" \
  CPPFLAGS="-DUNICODE -D_UNICODE -D_LFS64_LARGEFILE=1 -I$GCC494_SYSROOT/include" \
  LDFLAGS="-L$GCC494_SYSROOT/lib -static"

# Patch Makefile
cd src
sed -i 's|^CFLAGS = |CFLAGS = -O2 |'     Makefile
sed -i 's|^CXXFLAGS = |CXXFLAGS = -O2 |' Makefile
sed -i '/^CPPFLAGS = / { /-DUNICODE/! s|^CPPFLAGS = |CPPFLAGS = -DUNICODE -D_UNICODE | }' Makefile
# Remove system GCC 13 include/library paths that configure injects
sed -i 's|-I/usr/x86_64-w64-mingw32/include ||g' Makefile
sed -i 's|-L/usr/x86_64-w64-mingw32/lib ||g' Makefile

# Build
make -j$(nproc) \
  LIBS="-lFLAC -liconv -lssp -lws2_32 -ldxguid -lwinmm -ldinput -lole32 -ldsound -limm32 -lcfgmgr32 -loleaut32 -lsetupapi -lversion -lhid -lgdi32"

# Strip (unstripped binary has 21 sections + 106K COFF symbols, Windows rejects it)
x86_64-w64-mingw32-strip mednafen.exe

# Also save to stable location (native Linux rebuild does make clean which wipes src/)
cp -f mednafen.exe ../mednafen_gcc494.exe

echo "=== Output: $(ls -lh mednafen.exe 2>/dev/null || echo 'BUILD FAILED') ==="
echo "=== Compiler used ==="
strings mednafen.exe 2>/dev/null | grep "GCC:" | head -3
