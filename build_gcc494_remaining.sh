#!/bin/bash
# Stages 4-6 of GCC 4.9.4 cross-compiler build
set -e

PREFIX=/opt/gcc-4.9.4-mingw64
TARGET=x86_64-w64-mingw32
JOBS=$(nproc)
WORKDIR=$HOME/gcc494-build
MINGW_VER=v5.0.0

export PATH="$PREFIX/bin:$PATH"

echo "=== Verifying stage 3 GCC ==="
$TARGET-gcc --version | head -1

# Stage 4: mingw-w64 CRT
echo "=== Stage 4: mingw-w64 CRT ==="
mkdir -p "$WORKDIR/build/mingw-crt"
cd "$WORKDIR/build/mingw-crt"
if [ ! -f .done ]; then
  "$WORKDIR/src/mingw-w64-${MINGW_VER}/mingw-w64-crt/configure" \
    --host=$TARGET \
    --prefix="$PREFIX/$TARGET" \
    --with-sysroot="$PREFIX/$TARGET" \
    CC="$TARGET-gcc" \
    CXX="$TARGET-g++" \
    --disable-lib32 \
    --enable-lib64
  make -j$JOBS
  make install
  touch .done
fi

# Stage 5: winpthreads
echo "=== Stage 5: winpthreads ==="
mkdir -p "$WORKDIR/build/winpthreads"
cd "$WORKDIR/build/winpthreads"
if [ ! -f .done ]; then
  "$WORKDIR/src/mingw-w64-${MINGW_VER}/mingw-w64-libraries/winpthreads/configure" \
    --host=$TARGET \
    --prefix="$PREFIX/$TARGET" \
    CC="$TARGET-gcc" \
    CXX="$TARGET-g++" \
    --enable-static \
    --disable-shared
  make -j$JOBS
  make install
  touch .done
fi

# Stage 6: Full GCC (libgcc, libstdc++)
echo "=== Stage 6: Full GCC ==="
cd "$WORKDIR/build/gcc-core"
if [ ! -f .done-full ]; then
  make -j$JOBS CFLAGS="-std=gnu11 -w" CXXFLAGS="-std=gnu++14 -w"
  make install
  touch .done-full
fi

echo ""
echo "=== BUILD COMPLETE ==="
$PREFIX/bin/$TARGET-gcc --version | head -1
$PREFIX/bin/$TARGET-g++ --version | head -1
echo "Installed to: $PREFIX"
