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

# Debug mode: pass --debug as first argument
DEBUG_MODE=0
if [ "${1:-}" = "--debug" ]; then
  DEBUG_MODE=1
  echo "=== DEBUG BUILD (symbols, no strip, -O1) ==="
fi

# Patch ALL Makefiles (autotools generates per-directory Makefiles)
cd src
if [ "$DEBUG_MODE" = 1 ]; then
  # Replace standalone -O2 flag with -O1 -g in ALL Makefiles.
  # Use word boundary to avoid replacing -O2 inside other flags like -Wframe-larger-than.
  # Match -O2 preceded by space (or start) and followed by space.
  find . -name Makefile -exec sed -i 's| -O2 | -O1 -g |g' {} +
  # Also disable -Werror=write-strings and relax frame size warnings for debug
  find . -name Makefile -exec sed -i 's|-Wframe-larger-than=32768|-Wframe-larger-than=131072|g' {} +
  find . -name Makefile -exec sed -i 's|-Wstack-usage=32768|-Wstack-usage=131072|g' {} +
else
  sed -i 's|^CFLAGS = |CFLAGS = -O2 |'     Makefile
  sed -i 's|^CXXFLAGS = |CXXFLAGS = -O2 |' Makefile
fi
sed -i '/^CPPFLAGS = / { /-DUNICODE/! s|^CPPFLAGS = |CPPFLAGS = -DUNICODE -D_UNICODE | }' Makefile
# Remove system GCC 13 include/library paths that configure injects
sed -i 's|-I/usr/x86_64-w64-mingw32/include ||g' Makefile
sed -i 's|-L/usr/x86_64-w64-mingw32/lib ||g' Makefile

# Build
make -j$(nproc) \
  LIBS="-lFLAC -liconv -lssp -lws2_32 -ldxguid -lwinmm -ldinput -lole32 -ldsound -limm32 -lcfgmgr32 -loleaut32 -lsetupapi -lversion -lhid -lgdi32 -ldbghelp"

if [ "$DEBUG_MODE" = 1 ]; then
  # Separate debug symbols using objcopy (standard GNU approach):
  # 1. Extract debug info to .dbg file
  # 2. Strip the exe for runtime
  # 3. Link them so debuggers can find the symbols
  x86_64-w64-mingw32-objcopy --only-keep-debug mednafen.exe ../mednafen.dbg
  x86_64-w64-mingw32-strip --strip-debug mednafen.exe
  x86_64-w64-mingw32-objcopy --add-gnu-debuglink=../mednafen.dbg mednafen.exe
  cp -f mednafen.exe ../mednafen_debug.exe
  echo "=== Debug build: mednafen_debug.exe (stripped) + mednafen.dbg (symbols) ==="
  echo "=== Load both in GDB/WinDbg for full source-level debugging ==="
else
  # Release: strip (unstripped has too many COFF symbols for some Windows loaders)
  x86_64-w64-mingw32-strip mednafen.exe
  cp -f mednafen.exe ../mednafen_gcc494.exe
fi

echo "=== Output: $(ls -lh mednafen.exe 2>/dev/null || echo 'BUILD FAILED') ==="
echo "=== Compiler used ==="
strings mednafen.exe 2>/dev/null | grep "GCC:" | head -3
