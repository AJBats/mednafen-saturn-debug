# Mednafen Saturn Debugger - Command Reference

> **Note:** This document is intended as a reference for Claude and other coding LLMs
> that will be driving the debugger programmatically. Humans are welcome to read it too.
> For first-hand knowledge, read `src/drivers/automation.cpp` directly - it's the
> single source of truth for command parsing, ack formats, and IPC mechanics.

Custom fork of Mednafen with file-based automation and SH-2 debug tools.
Source at `src/drivers/automation.cpp`.

---

## Automation Mode

### Architecture

```
Windows Python script                    WSL Mednafen (--automation)
========================                 ===========================
Write mednafen_action.txt  --DrvFS-->    Poll action file each frame
                                         Execute command
Read mednafen_ack.txt      <--DrvFS--    Write ack with result + seq
```

- Mednafen starts **paused** at frame 0
- Action file uses content-based change detection (header line with sequence counter)
- Ack includes monotonic `seq=N` for reliable change detection
- Commands processed inside frame loop (frame-level) or CPU loop (instruction-level)

### Ack Format

**Every ack** has `cycle=<N> seq=<M>` appended by `write_ack()`. For example,
`ok frame_advance 1` is actually written as:

```
ok frame_advance 1 cycle=123456 seq=3
```

The ack formats in the tables below show the message portion only. When parsing,
use `seq=` for change detection and `cycle=` for timing analysis.

### Launching

```bash
# From WSL or native Linux:
export HOME="/tmp/mednafen_home" DISPLAY=:0 MEDNAFEN_ALLOWMULTI=1
rm -f "$HOME/.mednafen/mednafen.lck"
./mednafen --sound 0 --automation /tmp/mednafen_ipc /path/to/game.cue
```

```python
# Or from Windows Python, spawning into WSL:
launch_cmd = (
    f'export HOME="/tmp/mednafen_home" DISPLAY=:0 MEDNAFEN_ALLOWMULTI=1; '
    f'rm -f "$HOME/.mednafen/mednafen.lck"; '
    f'./mednafen --sound 0 --automation /tmp/mednafen_ipc /path/to/game.cue'
)
subprocess.Popen(["wsl", "-d", "Ubuntu", "-e", "bash", "-c", launch_cmd])
```

Key flags:
- `--automation <dir>` - enables automation mode, sets IPC directory
- `--sound 0` - disable audio (faster, no ALSA issues)
- `DISPLAY=:0` - required because WSLg doesn't propagate when spawned from Windows
- `MEDNAFEN_ALLOWMULTI=1` - allow multiple instances (for parallel comparison)
- Isolated `HOME` dir avoids lock file conflicts between instances

### Writing Commands (Python Client)

```python
# Atomic write with sequence counter for change detection
seq += 1
padding = "." * (seq % 16)
with open(tmp_path, "w", newline="\n") as f:
    f.write(f"# {seq}{padding}\n")  # header line (change detection)
    f.write(cmd + "\n")
os.replace(tmp_path, action_file)   # atomic rename (Python 3.3+)
```

### Reading Acks

```python
# Wait for ack to change from last known value
def wait_ack_change(timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ack = open(ack_file).read().strip()
        if ack != last_ack:
            return ack
        time.sleep(0.05)
    raise TimeoutError()
```

---

## Command Reference

### Frame Control

| Command | Description | Ack |
|---------|-------------|-----|
| `frame_advance [N]` | Run N frames (default 1), then pause | `ok frame_advance N` then `done frame_advance frame=N` |
| `run_to_frame N` | Free-run until frame N, then pause | `ok run_to_frame N` then `done run_to_frame frame=N` |
| `run` | Free-run (unpause) | `ok run` |
| `pause` | Pause emulation | `ok pause frame=N` |
| `quit` | Clean shutdown | `ok quit` |
| `status` | Report frame, pause state, breakpoints, input | `status frame=N paused=true/false ...` |

### Input

| Command | Description |
|---------|-------------|
| `input <button>` | Press button: START, A, B, C, X, Y, Z, UP, DOWN, LEFT, RIGHT, L, R |
| `input_release <button>` | Release button |
| `input_clear` | Release all buttons |

Button bits (Mednafen IDII layout):
- data[0]: Z(0) Y(1) X(2) R(3) UP(4) DOWN(5) LEFT(6) RIGHT(7)
- data[1]: B(0) C(1) A(2) START(3) pad(4-6) L(7)

### Debug: Registers & Memory

| Command | Description | Notes |
|---------|-------------|-------|
| `dump_regs` | Dump master SH-2 registers (text) | R0-R15, PC, SR, PR, GBR, VBR, MACH, MACL (23 values) |
| `dump_regs_bin <path>` | Write 22 uint32s to binary file | R0-R15, PC, SR, PR, GBR, VBR, MACH (no MACL). Little-endian. Read with `struct.unpack('<22I', data)` |
| `dump_slave_regs` | Dump slave SH-2 registers (text) | Same format as `dump_regs` but for slave CPU |
| `dump_slave_regs_bin <path>` | Write 22 uint32s for slave SH-2 | Same format as `dump_regs_bin` |
| `dump_mem <addr> <size>` | Hex dump memory (text, max 64KB) | Address in hex. Cache-aware reads. |
| `dump_mem_bin <addr> <size> <path>` | Write raw bytes to file (max 1MB) | Address and size in hex. |
| `dump_vdp2_regs <path>` | Write VDP2 register state to binary file | |
| `screenshot <path>` | Save framebuffer as PNG | Queued, taken next frame |

**Cache-aware memory reads**: `Automation_ReadMem8` checks the SH-2 instruction cache first
(tag match across 4 ways), falls back to backing RAM. This is critical - code loaded from
disc may only exist in cache, not in backing RAM.

### Debug: Instruction-Level

| Command | Description | Ack |
|---------|-------------|-----|
| `step [N]` | Execute N CPU instructions, then pause | `ok step N` then `done step pc=0xXXXXXXXX frame=N` |
| `breakpoint <addr>` | Add PC breakpoint (hex) | `ok breakpoint 0xXXXXXXXX total=N` |
| `breakpoint_clear` | Remove all breakpoints | `ok breakpoint_clear removed=N` |
| `breakpoint_list` | List active breakpoints | `breakpoints count=N 0xAAAAAAAA 0xBBBBBBBB` |
| `continue` | Resume until next breakpoint | `ok continue` then `break pc=0xXXXXXXXX ...` on hit |
| `dump_cycle` | Report current master cycle count | `ok dump_cycle value=N` |
| `run_to_cycle N` | Run until master cycle reaches N | `ok run_to_cycle target=N` then `done run_to_cycle ...` on hit |
| `deterministic` | Enable deterministic mode (fixed seed) | `ok deterministic` |

**How instruction-level pause works**: The SH-2 CPU debug hook (`Automation_DebugHook`)
runs on every instruction when active. On breakpoint hit or step completion, it spin-waits
inside the CPU loop. All debug commands (dump_regs, dump_mem, step, continue) work during
this pause because the action file is polled inside the spin-wait.

**Performance**: The CPU hook is enabled/disabled dynamically. When no breakpoints, steps,
or traces are active, overhead is zero. When active under software OpenGL (Mesa llvmpipe),
expect ~100x slowdown - use `--sound 0` and hidden window.

### Debug: Tracing

| Command | Description | Notes |
|---------|-------------|-------|
| `pc_trace_frame <path>` | Record every master PC for 1 frame | Binary file: sequence of uint32 PCs. ~320K entries/frame. |
| `call_trace <path>` | Log all JSR/BSR/BSRF calls to text file | Format: `<timestamp> M/S <caller_PC-4> <target_addr>` per line |
| `call_trace_stop` | Stop call trace logging | |
| `insn_trace <path> <start> <stop>` | Per-instruction trace to file | Traces between unified line numbers start..stop |
| `insn_trace_unified <start> <stop>` | Per-instruction trace into unified trace file | Uses lowercase `m/s` to distinguish from call events |
| `insn_trace_stop` | Stop instruction trace | |
| `unified_trace <path>` | Combined call trace + CD Block events to one file | Interleaves SH-2 calls (M/S) and CD Block events (CMD/DRV/IRQ/BUF) |
| `unified_trace_stop` | Stop unified trace | |
| `scdq_trace <path>` | Log SCDQ (Saturn CD queue) events | |
| `scdq_trace_stop` | Stop SCDQ trace | |
| `cdb_trace <path>` | Log CD Block events | |
| `cdb_trace_stop` | Stop CD Block trace | |
| `input_trace <path>` | Log button press/release events per frame | |
| `input_trace_stop` | Stop input trace | |

**Call trace format**: Each line is `<timestamp> M/S <caller_PC-4> <target_addr>` where
timestamp is the SH-2 cycle count, M = master, S = slave. In unified mode, instruction-level
lines use lowercase `m/s` with additional fields: `<timestamp> m/s <PC-4> <opcode> <MA_until> <mem_ts> <write_finish_ts> <sdram_finish> <CCR>`.

### Debug: Memory Watchpoint

| Command | Description |
|---------|-------------|
| `watchpoint <addr>` | Watch for 4-byte writes to addr (hex) |
| `watchpoint_clear` | Remove watchpoint |
| `vdp2_watchpoint <lo> <hi> <path>` | Watch for writes to VDP2 address range (hex), log to file |
| `vdp2_watchpoint_clear` | Remove VDP2 watchpoint |

**Two detection paths** (both required):
1. **CPU writes** - inline in `BusRW_DB_CS3` (ss.cpp), before/after value comparison
2. **SCU DMA writes** - inline in `DMA_Write` (scu.inc), same pattern

Watchpoint hits are **non-blocking** - logged to `watchpoint_hits.txt` and ack file.

File log format: `pc=0xXXXXXXXX pr=0xXXXXXXXX addr=0xXXXXXXXX old=0xXXXXXXXX new=0xXXXXXXXX frame=N`

Ack format (note - no `addr=` field): `hit watchpoint pc=0xXXXXXXXX pr=0xXXXXXXXX old=0xXXXXXXXX new=0xXXXXXXXX frame=N`

### Window Control

| Command | Description |
|---------|-------------|
| `show_window` | Make emulator window visible (for peeking) |
| `hide_window` | Hide window again |

Window starts hidden in automation mode. SDL_RaiseWindow is suppressed.

---

## Python Client Example

Minimal example showing how to launch, control, and debug:

```python
import os, time, subprocess

ipc_dir = "/tmp/mednafen_ipc"
action_file = os.path.join(ipc_dir, "mednafen_action.txt")
ack_file = os.path.join(ipc_dir, "mednafen_ack.txt")
seq = 0

def send(cmd, timeout=30):
    global seq
    seq += 1
    tmp = action_file + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(f"# {seq}\n{cmd}\n")
    os.replace(tmp, action_file)
    # Wait for ack
    deadline = time.time() + timeout
    last = open(ack_file).read().strip() if os.path.exists(ack_file) else ""
    while time.time() < deadline:
        ack = open(ack_file).read().strip()
        if ack != last:
            return ack
        time.sleep(0.05)
    raise TimeoutError(f"No ack for: {cmd}")

# Launch
os.makedirs(ipc_dir, exist_ok=True)
subprocess.Popen(["./mednafen", "--sound", "0",
                   "--automation", ipc_dir, "game.cue"])

# Use it
send("frame_advance 120")             # Advance 120 frames
send("screenshot /tmp/screen.png")    # Take screenshot
send("frame_advance 1")               # Screenshot is taken on next frame
send("breakpoint 06004000")           # Set breakpoint
send("continue")                      # Run to breakpoint
send("dump_regs")                     # Read registers
send("dump_mem 06000000 100")         # Hex dump 256 bytes
send("step 10")                       # Step 10 instructions
send("watchpoint 06010000")           # Watch memory address
send("quit")                          # Shutdown
```

---

## Common Debugging Patterns

### Pattern: Verify where execution hangs

```python
# 1. Set breakpoint at suspected hang location
send("breakpoint 06004000")
send("continue")
# If breakpoint fires -> function is reached
# If timeout -> hang is BEFORE this function

# 2. If reached, step through to find the polling loop
for i in range(100):
    ack = send("step 1")
    regs = send("dump_regs")
    print(f"Step {i}: {ack}")
```

### Pattern: Compare two builds at a function entry

Run two Mednafen instances with different disc images, same breakpoint:

```python
# Set breakpoint at target function in both instances
send_a("breakpoint 06004000")
send_b("breakpoint 06004000")
send_a("continue"); send_b("continue")

# Compare registers
send_a("dump_regs_bin /tmp/regs_a.bin")
send_b("dump_regs_bin /tmp/regs_b.bin")
# Read and compare the 22 uint32s
```

### Pattern: Find what writes to a memory address

```python
send("watchpoint 06010000")
send("run")
# Check watchpoint_hits.txt for: pc=... pr=... addr=... old=... new=... frame=...
```

### Pattern: Full 1MB memory comparison

```python
send_a("dump_mem_bin 06000000 100000 /tmp/ram_a.bin")
send_b("dump_mem_bin 06000000 100000 /tmp/ram_b.bin")
# Binary diff the two files
```

---

## WSLg Gotchas

- `$HOME` resolves to DrvFS mount when spawned from Windows Python - use isolated temp home
- `$DISPLAY` may be unset - always `export DISPLAY=:0`
- Lock file conflicts - `rm -f ~/.mednafen/mednafen.lck` before launch
- DrvFS stat() has 1-second mtime resolution - use content-based change detection
- Register dumps are **little-endian** (x86 host) - use `struct.unpack('<I', ...)`
