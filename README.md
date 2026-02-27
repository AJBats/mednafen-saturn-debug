# Mednafen Saturn Debug Fork

A fork of [Mednafen](https://mednafen.github.io/) with a file-based automation and debugging interface for the Sega Saturn. Built for use with AI agents (Claude) but works with any scripting language that can read/write files.

## What's Different

This fork adds `src/drivers/automation.cpp` - a file-based IPC interface that lets external scripts control the emulator at frame and instruction level:

- **Frame control** - advance N frames, run to frame, pause, resume
- **Input injection** - press/release Saturn controller buttons
- **Register dumps** - read all SH-2 registers (text or binary)
- **Memory dumps** - read arbitrary memory ranges (cache-aware)
- **Breakpoints** - set PC breakpoints, continue, step instructions
- **Memory watchpoints** - log writes to specific addresses
- **Tracing** - per-frame PC traces, call traces (JSR/BSR/BSRF)
- **Screenshots** - save framebuffer as PNG
- **Headless mode** - window starts hidden, so tests can run in the background without stealing focus

The interface uses two text files (`mednafen_action.txt` and `mednafen_ack.txt`) with sequence counters for reliable change detection over DrvFS.

## Building

Standard Mednafen build from WSL:

```bash
cd mednafen
./configure
make -j$(nproc)
```

The automation code compiles in unconditionally but is only active when launched with `--automation`.

## Usage

```bash
# Launch with automation enabled
./mednafen --sound 0 --automation /path/to/ipc/dir /path/to/game.cue
```

The emulator starts paused. Write commands to `mednafen_action.txt` in the IPC directory, read responses from `mednafen_ack.txt`.

## Command Reference

See [DEBUGGING.md](DEBUGGING.md) for the full command reference, Python client examples, and common debugging patterns.

## Origin

Forked from [libretro-mirrors/mednafen-git](https://github.com/libretro-mirrors/mednafen-git). All automation additions are in `src/drivers/automation.cpp` and `src/drivers/automation.h`, with minimal hooks into the existing Saturn and driver code.
