# Building Mednafen for Windows (GCC 4.9.4 cross-compile from WSL)

Our custom Mednafen fork with automation/CDL hooks, built as a native Windows PE32+ exe.
Uses GCC 4.9.4 to match the stock Mednafen 1.32.1 build environment.

## Quick Start (copy-paste this)

```bash
# Step 1: If /opt/gcc-4.9.4-mingw64/ does NOT exist, build the cross-compiler first:
sudo apt install build-essential texinfo bison flex mingw-w64 libsdl2-dev
cd /mnt/d/Projects/SaturnReverseTest/mednafen
cat build_gcc494_cross.sh | tr -d '\r' | bash
cat build_gcc494_remaining.sh | tr -d '\r' | bash
# This takes ~20 minutes. Only needed once.

# Step 2: Build Mednafen
cd /mnt/d/Projects/SaturnReverseTest/mednafen
cat build_with_gcc494.sh | tr -d '\r' | bash

# Output: mednafen/src/mednafen.exe (~23MB)
```

**IMPORTANT**: Run scripts via `cat ... | tr -d '\r' | bash`, NOT `bash script.sh`.
Git on Windows adds CRLF line endings that break the scripts.

---

## Details

### One-time: Build GCC 4.9.4 cross-compiler

The cross-compiler installs to `/opt/gcc-4.9.4-mingw64/` in WSL. Takes ~20 minutes.

```bash
# Install build dependencies
sudo apt install build-essential texinfo bison flex

# Build stages 1-3 (binutils, mingw-w64 headers, GCC core)
bash build_gcc494_cross.sh

# Build stages 4-6 (mingw-w64 CRT, winpthreads, full GCC)
bash build_gcc494_remaining.sh

# Verify
/opt/gcc-4.9.4-mingw64/bin/x86_64-w64-mingw32-g++ --version
# Should print: x86_64-w64-mingw32-g++ (GCC) 4.9.4
```

The bootstrap builds GCC 4.9.4 from source with binutils 2.25.1 and mingw-w64 v5.0.0.
GCC 4.9.4 source uses `bool++` which is forbidden in C++17, so the scripts force
`-std=gnu++14` when building with the host GCC 13.

### System libraries (Ubuntu/WSL)

```bash
sudo apt install mingw-w64 libsdl2-dev
```

SDL2, zlib, FLAC, and iconv static libraries come from the system mingw-w64 package.
The build script copies them into the GCC 4.9.4 sysroot automatically.

## Build

```bash
cd /mnt/d/Projects/SaturnReverseTest/mednafen
bash build_with_gcc494.sh
```

Output: `mednafen/src/mednafen.exe` (~23MB stripped PE32+ executable).

The script handles everything: configure, Makefile patching, automation stubs,
stripping. After the first configure, incremental rebuilds only recompile changed files.

### Quick rebuild (after source changes)

```bash
export PATH="/opt/gcc-4.9.4-mingw64/bin:$PATH"
cd /mnt/d/Projects/SaturnReverseTest/mednafen/src
make -j$(nproc) \
  LIBS="ss/automation_stubs.o -lFLAC -liconv -lssp -lws2_32 -ldxguid -lwinmm -ldinput -lole32 -ldsound -limm32 -lcfgmgr32 -loleaut32 -lsetupapi -lversion -lhid -lgdi32"
x86_64-w64-mingw32-strip mednafen.exe
```

## Running

Set `MEDNAFEN_HOME` to share config/firmware with the stock install (one-time setup
in System Properties > Environment Variables):

```
MEDNAFEN_HOME = C:\Users\albat\.mednafen
```

Then:
```
mednafen\src\mednafen.exe "path\to\game.cue"
```

## What the build script does

1. Copies SDL2/FLAC/zlib/iconv static libs into GCC 4.9.4 sysroot
2. Replaces GCC 4.9.4's `libmsvcrt.a` with system's (adds `_wassert`, `__acrt_iob_func`)
3. Runs `./configure` with `--disable-sdltest` (WSL runs .exe, fools cross-compile detection)
4. Patches Makefile to remove system GCC 13 include/library paths that configure injects
5. Compiles `automation_stubs.cpp` (stock ss.cpp doesn't have automation hooks)
6. Builds with `-lssp` (FLAC lib uses stack protector) and `-liconv`
7. Strips the binary (unstripped has 21 sections + 106K COFF symbols; Windows rejects it)

## Gotchas

1. **Must strip**: The unstripped binary has too many COFF sections/symbols for
   Windows to load. Always strip after linking.

2. **System paths leak into Makefile**: Configure detects system mingw-w64 at
   `/usr/x86_64-w64-mingw32/` and injects its include/lib paths. The build script
   removes these with `sed` so only GCC 4.9.4's sysroot is used.

3. **WSL defeats cross-compile detection**: WSL can run Windows .exe files, so
   configure's cross-compilation test passes and it tries to run SDL test programs.
   `--disable-sdltest` skips this.

4. **sdl2-config must handle multiple args**: Configure calls
   `sdl2-config --prefix=... --version`. The wrapper iterates all args with
   `for arg in "$@"` instead of checking only `$1`.

5. **Config directory**: On Windows, Mednafen checks `MEDNAFEN_HOME` first, then
   `HOME/.mednafen`, then the exe's own directory. Without `MEDNAFEN_HOME`, firmware
   won't be found.
