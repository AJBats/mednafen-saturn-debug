#!/bin/bash
# Build GCC 4.9.4 cross-compiler targeting x86_64-w64-mingw32
# Run from WSL Ubuntu. Installs to /opt/gcc-4.9.4-mingw64
set -e

PREFIX=/opt/gcc-4.9.4-mingw64
TARGET=x86_64-w64-mingw32
JOBS=$(nproc)
WORKDIR=$HOME/gcc494-build

# Versions (matching stock Mednafen build environment)
GCC_VER=4.9.4
BINUTILS_VER=2.25.1
MINGW_VER=v5.0.0    # closest available release to v5.0.5 runtime
GMP_VER=5.1.3
MPFR_VER=3.1.6
MPC_VER=1.0.3

echo "=== Building GCC $GCC_VER cross-compiler for $TARGET ==="
echo "=== Install prefix: $PREFIX ==="
echo "=== Build dir: $WORKDIR ==="
echo "=== Using $JOBS parallel jobs ==="

# Create dirs
sudo mkdir -p "$PREFIX"
sudo chown $(whoami) "$PREFIX"
mkdir -p "$WORKDIR/src" "$WORKDIR/build"
cd "$WORKDIR/src"

# Download sources
echo "=== Downloading sources ==="
[ -f gcc-${GCC_VER}.tar.gz ] || wget -q https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.gz
[ -f binutils-${BINUTILS_VER}.tar.gz ] || wget -q https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.gz
[ -f mingw-w64-${MINGW_VER}.tar.bz2 ] || wget -q "https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-${MINGW_VER}.tar.bz2/download" -O mingw-w64-${MINGW_VER}.tar.bz2
[ -f gmp-${GMP_VER}.tar.xz ] || wget -q https://ftp.gnu.org/gnu/gmp/gmp-${GMP_VER}.tar.xz
[ -f mpfr-${MPFR_VER}.tar.xz ] || wget -q https://ftp.gnu.org/gnu/mpfr/mpfr-${MPFR_VER}.tar.xz
[ -f mpc-${MPC_VER}.tar.gz ] || wget -q https://ftp.gnu.org/gnu/mpc/mpc-${MPC_VER}.tar.gz

# Extract
echo "=== Extracting sources ==="
[ -d gcc-${GCC_VER} ] || tar xf gcc-${GCC_VER}.tar.gz
[ -d binutils-${BINUTILS_VER} ] || tar xf binutils-${BINUTILS_VER}.tar.gz
[ -d mingw-w64-${MINGW_VER} ] || tar xjf mingw-w64-${MINGW_VER}.tar.bz2
[ -d gmp-${GMP_VER} ] || tar xf gmp-${GMP_VER}.tar.xz
[ -d mpfr-${MPFR_VER} ] || tar xf mpfr-${MPFR_VER}.tar.xz
[ -d mpc-${MPC_VER} ] || tar xf mpc-${MPC_VER}.tar.gz

# Link GMP/MPFR/MPC into GCC source tree (so GCC builds them automatically)
cd gcc-${GCC_VER}
[ -L gmp ] || ln -s ../gmp-${GMP_VER} gmp
[ -L mpfr ] || ln -s ../mpfr-${MPFR_VER} mpfr
[ -L mpc ] || ln -s ../mpc-${MPC_VER} mpc
cd ..

export PATH="$PREFIX/bin:$PATH"

# GCC 4.9.4 doesn't compile with GCC 13's default C++17 mode.
# Force C++14 and suppress warnings-as-errors.
export CFLAGS="-std=gnu11 -Wno-error -w"
export CXXFLAGS="-std=gnu++14 -Wno-error -w"

# ============================================================
# Stage 1: binutils
# ============================================================
echo "=== Stage 1: binutils ==="
mkdir -p "$WORKDIR/build/binutils"
cd "$WORKDIR/build/binutils"
if [ ! -f .done ]; then
  "$WORKDIR/src/binutils-${BINUTILS_VER}/configure" \
    --target=$TARGET \
    --prefix="$PREFIX" \
    --with-sysroot="$PREFIX/$TARGET" \
    --disable-multilib \
    --disable-nls \
    --disable-werror
  make -j$JOBS
  make install
  touch .done
fi

# ============================================================
# Stage 2: mingw-w64 headers
# ============================================================
echo "=== Stage 2: mingw-w64 headers ==="
mkdir -p "$PREFIX/$TARGET/include"
mkdir -p "$WORKDIR/build/mingw-headers"
cd "$WORKDIR/build/mingw-headers"
if [ ! -f .done ]; then
  "$WORKDIR/src/mingw-w64-${MINGW_VER}/mingw-w64-headers/configure" \
    --host=$TARGET \
    --prefix="$PREFIX/$TARGET" \
    --enable-sdk=all
  make install
  # GCC expects a symlink
  [ -L "$PREFIX/mingw" ] || ln -s "$PREFIX/$TARGET" "$PREFIX/mingw"
  touch .done
fi

# ============================================================
# Stage 3: GCC core (C only, no libc yet)
# ============================================================
echo "=== Stage 3: GCC core (C only) ==="
mkdir -p "$WORKDIR/build/gcc-core"
cd "$WORKDIR/build/gcc-core"
if [ ! -f .done ]; then
  "$WORKDIR/src/gcc-${GCC_VER}/configure" \
    --target=$TARGET \
    --prefix="$PREFIX" \
    --with-sysroot="$PREFIX/$TARGET" \
    --disable-multilib \
    --disable-nls \
    --disable-shared \
    --disable-threads \
    --disable-werror \
    --enable-languages=c,c++ \
    --enable-fully-dynamic-string \
    --with-gnu-as \
    --with-gnu-ld \
    CFLAGS="-std=gnu11 -Wno-error -w" \
    CXXFLAGS="-std=gnu++14 -Wno-error -w" \
    CFLAGS_FOR_TARGET="-O2" \
    CXXFLAGS_FOR_TARGET="-O2"
  make -j$JOBS all-gcc CFLAGS="-std=gnu11 -w" CXXFLAGS="-std=gnu++14 -w"
  make install-gcc
  touch .done
fi

# ============================================================
# Stage 4: mingw-w64 CRT (needs the stage 1 GCC)
# ============================================================
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

# ============================================================
# Stage 5: winpthreads (needed for C++ threading support)
# ============================================================
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

# ============================================================
# Stage 6: Full GCC (libgcc, libstdc++, etc.)
# ============================================================
echo "=== Stage 6: Full GCC build ==="
cd "$WORKDIR/build/gcc-core"
if [ ! -f .done-full ]; then
  make -j$JOBS CFLAGS="-std=gnu11 -w" CXXFLAGS="-std=gnu++14 -w"
  make install
  touch .done-full
fi

echo ""
echo "=== BUILD COMPLETE ==="
echo "Toolchain installed to: $PREFIX"
echo ""
$PREFIX/bin/$TARGET-gcc --version
echo ""
echo "Add to PATH: export PATH=$PREFIX/bin:\$PATH"
