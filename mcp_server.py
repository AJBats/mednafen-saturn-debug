#!/usr/bin/env python3
"""Mednafen MCP Server v2 — full debug bridge for Claude Code.

Architecture: Claude Code --(stdio/MCP)--> this server --(file IPC)--> Mednafen (Windows)

All tools follow the same pattern:
  1. Check _alive()
  2. _send_and_wait(command, ack_keyword)
  3. Return result string

Game-agnostic. Game-specific helpers belong in the consuming project.
"""

from __future__ import annotations
import os
import sys
import re
import time
import asyncio
import subprocess
import tempfile
import struct

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("mednafen")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def wsl_path(win_path):
    """Normalize path for the emulator (forward slashes)."""
    if not win_path:
        return win_path
    return win_path.replace("\\", "/")


def _strip_hex(s):
    """Remove 0x/0X prefix from hex string."""
    return s.replace("0x", "").replace("0X", "")


# ---------------------------------------------------------------------------
# Session state (module-level globals)
# ---------------------------------------------------------------------------

_proc = None
_ack_file = None
_action_file = None
_last_ack = ""
_seq = 0
_frame = 0
_ipc_dir = None
_default_cue = None
_home_dir = None

# Cheat-engine memory snapshots (name -> bytes)
_memory_snapshots = {}

# Track whether any break-producing instruments are active, so run_free
# can auto-detect whether to wait for a break event.
_break_sources = set()  # e.g. {"breakpoint", "watchpoint", "read_watchpoint", "exception"}
_active_breakpoints = set()  # Individual breakpoint addresses (hex strings)


def _send(cmd):
    """Write a command to the action file (with retry for Windows file locks)."""
    global _seq
    _seq += 1
    padding = "." * (_seq % 16)
    tmp = _action_file + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(f"# {_seq}{padding}\n")
        f.write(cmd + "\n")
    for _retry in range(20):
        try:
            if os.path.exists(_action_file):
                os.remove(_action_file)
            os.rename(tmp, _action_file)
            return
        except PermissionError:
            time.sleep(0.02 * (_retry + 1))
    # Final attempt without catching
    if os.path.exists(_action_file):
        os.remove(_action_file)
    os.rename(tmp, _action_file)


async def _wait_ack(keyword, timeout=30):
    """Poll ack file for a line containing keyword.
    keyword can be a string or a list of strings (matches any)."""
    global _last_ack
    keywords = [keyword] if isinstance(keyword, str) else keyword
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _proc and _proc.poll() is not None:
            return None
        if _ack_file and os.path.exists(_ack_file):
            try:
                with open(_ack_file) as f:
                    content = f.read().strip()
            except (IOError, PermissionError):
                await asyncio.sleep(0.05)
                continue
            if content != _last_ack and any(k in content for k in keywords):
                _last_ack = content
                return content
        await asyncio.sleep(0.05)
    return None


async def _send_and_wait(cmd, keyword, timeout=30):
    """Send command, wait for ack."""
    _send(cmd)
    return await _wait_ack(keyword, timeout)


def _alive():
    return _proc is not None and _proc.poll() is None


def _ipc_path(name):
    """Return a path inside the IPC directory for temp files."""
    return os.path.join(_ipc_dir or tempfile.gettempdir(), name)


# ---------------------------------------------------------------------------
# Core tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def boot(cue_path: str = "", timeout: int = 45, sound: bool = False) -> str:
    """Launch Mednafen with a disc image. Starts paused at frame 0.
    Set sound=True for interactive play (enables audio and frame pacing)."""
    global _proc, _ack_file, _action_file, _last_ack, _seq, _frame, _ipc_dir

    if _alive():
        _send("quit")
        try:
            _proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _proc.kill()

    _break_sources.clear()
    _active_breakpoints.clear()
    cue = cue_path or _default_cue or ""
    if not cue:
        return "FAIL: No disc image. Pass cue_path or use --cue."

    ipc = _ipc_dir or os.path.join(tempfile.gettempdir(), "mednafen_mcp_ipc")
    os.makedirs(ipc, exist_ok=True)
    _ipc_dir = ipc
    _ack_file = os.path.join(ipc, "mednafen_ack.txt")
    _action_file = os.path.join(ipc, "mednafen_action.txt")

    for f in [_ack_file, _action_file]:
        if os.path.exists(f):
            os.remove(f)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    # Prefer debug build (has crash dump handler + symbols), fall back to release
    med_debug = os.path.join(script_dir, "mednafen_debug.exe")
    med_primary = os.path.join(script_dir, "src", "mednafen.exe")
    med_fallback = os.path.join(script_dir, "mednafen_gcc494.exe")
    if os.path.exists(med_debug):
        med_bin = med_debug
    elif os.path.exists(med_primary):
        med_bin = med_primary
    else:
        med_bin = med_fallback

    # Project-local MEDNAFEN_HOME so multiple instances can run side-by-side.
    # Priority: --home-dir flag > MEDNAFEN_HOME env var > script_dir/home default
    med_home = _home_dir or os.environ.get("MEDNAFEN_HOME") or os.path.join(script_dir, "home")
    os.makedirs(med_home, exist_ok=True)

    # Remove stale lockfile
    lockfile = os.path.join(med_home, "mednafen.lck")
    if os.path.exists(lockfile):
        os.remove(lockfile)

    env = os.environ.copy()
    env["MEDNAFEN_HOME"] = med_home
    # Point crash dumps to SaturnAutoRE/crash_dumps/
    crash_dir = os.path.join(script_dir, "..", "crash_dumps")
    os.makedirs(crash_dir, exist_ok=True)
    env["MEDNAFEN_CRASH_DUMP_DIR"] = os.path.abspath(crash_dir)

    stderr_f = tempfile.NamedTemporaryFile(mode="w", suffix="_med.txt", delete=False)
    _proc = subprocess.Popen(
        [med_bin, "--sound", "1" if sound else "0",
         "-cd.image_memcache", "1",
         "--automation", ipc, cue],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=stderr_f,
        env=env,
    )

    _last_ack = ""
    _seq = 0
    _frame = 0
    _memory_snapshots.clear()

    deadline = time.time() + timeout
    while time.time() < deadline:
        if _proc.poll() is not None:
            return f"FAIL: Mednafen exited with code {_proc.returncode}"
        if os.path.exists(_ack_file):
            try:
                with open(_ack_file) as f:
                    content = f.read().strip()
                if "ready" in content:
                    _last_ack = content
                    return "OK: Mednafen ready. Emulator paused at frame 0."
            except (IOError, PermissionError):
                pass
        await asyncio.sleep(0.2)

    return "FAIL: Timeout waiting for Mednafen"


@mcp.tool()
async def status() -> str:
    """Get emulator status (frame, pause state, breakpoints, input)."""
    if not _alive():
        return "No session running."
    ack = await _send_and_wait("status", "status", timeout=5)
    return f"{ack or ''}"


@mcp.tool()
async def frame_advance(count: int = 1) -> str:
    """Advance N frames (default 1). Emulator pauses after."""
    global _frame
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"frame_advance {count}",
                               ["done frame_advance", "hit watchpoint", "hit read_watchpoint", "break pc=", "hit exception"],
                               timeout=180)
    if ack:
        if "hit watchpoint" in ack or "hit read_watchpoint" in ack:
            return ack
        if "hit exception" in ack:
            return f"EXCEPTION: {ack}"
        if "break pc=" in ack:
            return f"BREAK: {ack}"
        # Parse real frame from automation ack (e.g. "done frame_advance frame=123")
        m = re.search(r"frame=(\d+)", ack)
        if m:
            _frame = int(m.group(1))
        else:
            _frame += count
        return f"OK: Advanced {count} frames. Now at frame {_frame}"
    return "FAIL: frame_advance timed out"


@mcp.tool()
async def screenshot(output_path: str = "", scale: int = 3) -> str:
    """Screenshot from cached framebuffer (no PC movement). Returns file path.
    Scale parameter upscales the image (default 3x) for better readability."""
    if not _alive():
        return "FAIL: No session"
    shot = output_path or os.path.join(os.getcwd(), "mcp_screenshot.png")
    if os.path.exists(shot):
        os.remove(shot)
    ack = await _send_and_wait(f"screenshot {wsl_path(shot)}", "ok screenshot", timeout=10)
    if not ack:
        return "FAIL: screenshot timed out"
    if "error" in ack:
        return f"FAIL: {ack}"
    if scale > 1 and os.path.exists(shot):
        try:
            from PIL import Image
            img = Image.open(shot)
            img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
            img.save(shot)
        except Exception:
            pass  # If PIL unavailable, keep original size
    size = os.path.getsize(shot) if os.path.exists(shot) else 0
    return f"OK: Screenshot saved to {shot} ({size} bytes, {scale}x)"


@mcp.tool()
async def quit_emulator() -> str:
    """Shutdown Mednafen."""
    global _proc
    if _alive():
        _send("quit")
        try:
            _proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _proc.kill()
    _proc = None
    _break_sources.clear()
    _active_breakpoints.clear()
    return "OK"


# ---------------------------------------------------------------------------
# Input
# ---------------------------------------------------------------------------

@mcp.tool()
async def input_press(button: str) -> str:
    """Press and hold a button. Stays held until input_release.
    Buttons: START, A, B, C, X, Y, Z, UP, DOWN, LEFT, RIGHT, L, R.
    """
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"input {button}", "ok input", timeout=5)
    return f"OK: Pressed {button}" if ack else f"FAIL: input {button} timed out"


@mcp.tool()
async def input_release(button: str) -> str:
    """Release a held button."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"input_release {button}", "ok input_release", timeout=5)
    return f"OK: Released {button}" if ack else f"FAIL: input_release {button}"


@mcp.tool()
async def input_clear() -> str:
    """Release all held buttons."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("input_clear", "ok input_clear", timeout=5)
    return "OK: All buttons released" if ack else "FAIL: input_clear timed out"


@mcp.tool()
async def input_tap(button: str, hold_frames: int = 5) -> str:
    """Press a button for N frames then release. Convenience for menu navigation."""
    global _frame
    if not _alive():
        return "FAIL: No session"
    await _send_and_wait(f"input {button}", "ok input", timeout=5)
    ack = await _send_and_wait(f"frame_advance {hold_frames}", "done frame_advance", timeout=30)
    if ack:
        _frame += hold_frames
    await _send_and_wait(f"input_release {button}", "ok input_release", timeout=5)
    return f"OK: Tapped {button} for {hold_frames} frames. Now at frame {_frame}"


# ---------------------------------------------------------------------------
# Memory reading
# ---------------------------------------------------------------------------

@mcp.tool()
async def read_memory(address: str, length: int = 64) -> str:
    """Read memory as hex dump text. Address in hex (e.g. '0x06078900'). Max 4096 bytes."""
    if not _alive():
        return "FAIL: No session"
    length = min(length, 4096)
    addr = _strip_hex(address)
    ack = await _send_and_wait(f"dump_mem {addr} {length:x}", "mem ", timeout=10)
    return ack if ack else "FAIL: dump_mem timed out"


@mcp.tool()
async def read_memory_binary(address: str, length: int = 256) -> str:
    """Read memory as hex+ASCII dump. Max 65536 bytes. Uses dump_mem_bin internally."""
    if not _alive():
        return "FAIL: No session"
    length = min(length, 65536)
    addr = _strip_hex(address)
    out_path = _ipc_path("mem_dump.bin")
    ack = await _send_and_wait(
        f"dump_mem_bin {addr} {length:x} {wsl_path(out_path)}",
        "dump_mem_bin", timeout=10,
    )
    if not ack:
        return "FAIL: dump_mem_bin timed out"
    await asyncio.sleep(0.2)
    if not os.path.exists(out_path):
        return "FAIL: dump file not created"
    with open(out_path, "rb") as f:
        data = f.read()
    base_addr = int(addr, 16)
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {base_addr + i:08X}: {hex_part:<48s} {ascii_part}")
    return "\n".join(lines)


@mcp.tool()
async def read_u32(address: str) -> str:
    """Read a 32-bit big-endian value. Returns hex, unsigned, and signed decimal."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    out_path = _ipc_path("u32_dump.bin")
    ack = await _send_and_wait(f"dump_mem_bin {addr} 4 {wsl_path(out_path)}", "dump_mem_bin", timeout=10)
    if not ack:
        return "FAIL: timed out"
    await asyncio.sleep(0.2)
    if not os.path.exists(out_path):
        return "FAIL: dump file not created"
    with open(out_path, "rb") as f:
        data = f.read(4)
    if len(data) < 4:
        return f"FAIL: only got {len(data)} bytes"
    val = struct.unpack(">I", data)[0]
    signed = struct.unpack(">i", data)[0]
    return f"0x{addr}: 0x{val:08X} (unsigned={val}, signed={signed})"


@mcp.tool()
async def read_u16(address: str) -> str:
    """Read a 16-bit big-endian value. Returns hex, unsigned, and signed decimal."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    out_path = _ipc_path("u16_dump.bin")
    ack = await _send_and_wait(f"dump_mem_bin {addr} 2 {wsl_path(out_path)}", "dump_mem_bin", timeout=10)
    if not ack:
        return "FAIL: timed out"
    await asyncio.sleep(0.2)
    if not os.path.exists(out_path):
        return "FAIL: dump file not created"
    with open(out_path, "rb") as f:
        data = f.read(2)
    if len(data) < 2:
        return f"FAIL: only got {len(data)} bytes"
    val = struct.unpack(">H", data)[0]
    signed = struct.unpack(">h", data)[0]
    return f"0x{addr}: 0x{val:04X} (unsigned={val}, signed={signed})"


# ---------------------------------------------------------------------------
# Registers
# ---------------------------------------------------------------------------

@mcp.tool()
async def dump_regs() -> str:
    """Dump primary SH-2 registers (R0-R15, PC, SR, PR, GBR, VBR, MACH, MACL)."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("dump_regs", "R0=", timeout=10)
    return ack if ack else "FAIL: dump_regs timed out"


@mcp.tool()
async def dump_slave_regs() -> str:
    """Dump secondary SH-2 registers."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("dump_slave_regs", "R0=", timeout=10)
    return ack if ack else "FAIL: dump_slave_regs timed out"


@mcp.tool()
async def dump_regs_binary() -> str:
    """Dump primary SH-2 registers to binary file and return formatted output."""
    if not _alive():
        return "FAIL: No session"
    out_path = _ipc_path("regs.bin")
    ack = await _send_and_wait(f"dump_regs_bin {wsl_path(out_path)}", "dump_regs_bin", timeout=10)
    if not ack:
        return "FAIL: timed out"
    await asyncio.sleep(0.2)
    if not os.path.exists(out_path):
        return "FAIL: file not created"
    with open(out_path, "rb") as f:
        data = f.read()
    if len(data) < 88:
        return f"FAIL: expected 88 bytes, got {len(data)}"
    regs = struct.unpack("<22I", data)
    names = [f"R{i}" for i in range(16)] + ["PC", "SR", "PR", "GBR", "VBR", "MACH"]
    lines = []
    for i, name in enumerate(names):
        lines.append(f"  {name:5s} = 0x{regs[i]:08X}")
    return "\n".join(lines)


@mcp.tool()
async def call_stack(scan_size: int = 1024) -> str:
    """Heuristic SH-2 call stack. Scans stack for return addresses in code regions."""
    if not _alive():
        return "FAIL: No session"
    sz = min(scan_size, 4096)
    ack = await _send_and_wait(f"call_stack {sz:x}", "call_stack", timeout=10)
    return ack if ack else "FAIL: call_stack timed out"


# ---------------------------------------------------------------------------
# Breakpoints & stepping
# ---------------------------------------------------------------------------

@mcp.tool()
async def breakpoint_set(address: str, log: bool = False) -> str:
    """Set a PC breakpoint. Address in hex (e.g. '0x0600C5D6').
    If log=True, logs full context (registers + call stack) to
    breakpoint_hits.txt without pausing. Use for surveying all callers."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    cmd = f"breakpoint {addr}"
    if log:
        cmd += " log"
    ack = await _send_and_wait(cmd, "ok breakpoint", timeout=5)
    if ack and not log:
        _active_breakpoints.add(addr)
        _break_sources.add("breakpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_remove(address: str) -> str:
    """Remove a single breakpoint."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    ack = await _send_and_wait(f"breakpoint_remove {addr}", "breakpoint_remove", timeout=5)
    if ack:
        _active_breakpoints.discard(addr)
        if not _active_breakpoints:
            _break_sources.discard("breakpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_clear() -> str:
    """Remove all breakpoints."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("breakpoint_clear", "breakpoint_clear", timeout=5)
    if ack:
        _active_breakpoints.clear()
        _break_sources.discard("breakpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_list() -> str:
    """List all active breakpoints."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("breakpoint_list", "breakpoints", timeout=5)
    return ack if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# Poke triggers: write memory on breakpoint hit, continue without pausing.
# Enables auto-replay of captured car-struct (or any memory region) state
# at PC-aligned moments in each frame, driven by a CSV of per-frame values.
# ---------------------------------------------------------------------------

def _parse_width(w):
    w = int(w)
    if w not in (8, 16, 32):
        raise ValueError(f"width must be 8/16/32, got {w}")
    return w


@mcp.tool()
async def poke_breakpoint_set(trigger_pc: str, pokes: list[dict]) -> str:
    """Install a PC-triggered poke: on every hit at trigger_pc, write the given
    list of memory values atomically and continue without pausing.

    pokes is a list of dicts: {addr: hex, value: int|hex, width: 8|16|32}.
    Example: pokes=[{"addr": "0x0605224C", "value": 0x008CF8D0, "width": 32}]

    Replaces any existing trigger at trigger_pc (static or playback)."""
    if not _alive():
        return "FAIL: No session"
    if not pokes:
        return "FAIL: pokes list is empty"
    tpc = _strip_hex(trigger_pc)
    parts = [f"poke_breakpoint {tpc} {len(pokes)}"]
    for p in pokes:
        addr = _strip_hex(str(p["addr"]))
        v = p["value"]
        vhex = _strip_hex(v) if isinstance(v, str) else f"{int(v):X}"
        w = _parse_width(p["width"])
        parts.append(f"{addr}:{vhex}:{w}")
    ack = await _send_and_wait(" ".join(parts),
                               ["ok poke_breakpoint", "error poke_breakpoint", "error unknown command"],
                               timeout=5)
    if ack and ack.startswith("ok poke_breakpoint"):
        _break_sources.add("poke_trigger")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_breakpoint_remove(trigger_pc: str) -> str:
    """Remove one poke trigger by PC. If this leaves no triggers, run_free
    will no longer auto-wait for poke-related breaks."""
    if not _alive():
        return "FAIL: No session"
    tpc = _strip_hex(trigger_pc)
    ack = await _send_and_wait(f"poke_breakpoint_remove {tpc}",
                               ["ok poke_breakpoint_remove", "error poke_breakpoint_remove", "error unknown command"],
                               timeout=5)
    # Ack includes "total_triggers=N" — discard the break source when N==0
    # so the next run_free doesn't block on a break that can't happen.
    if ack and "total_triggers=0" in ack:
        _break_sources.discard("poke_trigger")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_breakpoint_clear() -> str:
    """Remove all poke triggers (static and playback)."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("poke_breakpoint_clear",
                               ["ok poke_breakpoint_clear", "error unknown command"], timeout=5)
    if ack and ack.startswith("ok"):
        _break_sources.discard("poke_trigger")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_breakpoint_list() -> str:
    """List active poke triggers (static + playback, with hit counters)."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("poke_breakpoint_list",
                               ["poke_triggers", "error unknown command"], timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_playback_start(trigger_pc: str, base_addr: str, csv_path: str,
                              columns: list[dict], start_row: int = 0,
                              end_row: int = -1, on_end: str = "hold") -> str:
    """Drive memory writes from a CSV, one row per hit at trigger_pc.

    Typical use: replay per-frame car-struct captures at a once-per-frame PC
    in a transplant build so downstream rendering sees retail-like values.

    Args:
      trigger_pc: hex PC, e.g. "0x06028002". Pokes fire right before this
                  instruction executes.
      base_addr:  hex base address the column offsets are relative to
                  (e.g. "0x0605224C" for car[0]).
      csv_path:   absolute path to the CSV. Format is the one produced by
                  sample_memory + tools/blob_to_csv.py: header row with
                  "+0xNN" offset names, data rows with 0xHHHHHHHH cells.
                  Must be resolvable from Mednafen's working directory
                  (use forward slashes). No spaces in the path.
      columns:    list of dicts. Each entry:
                    {csv_column: "+0x00", offset: 0x00, width: 32}
                    {csv_column: "+0x0C", offset: 0x0E, width: 16,
                     byte_slice: [2, 4]}   # low 16 of a BE32 cell
      start_row:  first CSV data row to play (default 0)
      end_row:    one past last row; -1 means EOF (default)
      on_end:     "halt" (pause + ack), "loop" (wrap to start_row),
                  "hold" (freeze on last row, keep poking)

    Replaces any existing trigger at trigger_pc. Only one playback cursor at
    a time — starting a new one supersedes the old.

    After this, call run_free(wait_for_break=True) to drive the emulator
    until the playback halts (if on_end="halt") or until another break fires.

    Note on save states: poke trigger state lives outside Mednafen's save-state
    system. If you save_state during playback and load_state later, the cursor
    keeps its current position rather than rewinding to the saved moment.
    For reproducible runs, call poke_playback_start fresh after load_state."""
    if not _alive():
        return "FAIL: No session"
    if not columns:
        return "FAIL: columns list is empty"
    if on_end not in ("halt", "loop", "hold"):
        return f"FAIL: on_end must be halt|loop|hold, got {on_end}"
    tpc  = _strip_hex(trigger_pc)
    base = _strip_hex(base_addr)
    parts = [f"poke_playback_start {tpc} {base} {int(start_row)} {int(end_row)} {on_end} {len(columns)}"]
    for col in columns:
        name = str(col["csv_column"])
        if ":" in name or " " in name:
            return f"FAIL: csv_column name cannot contain ':' or spaces (got {name!r})"
        off = _strip_hex(str(col["offset"])) if isinstance(col["offset"], str) else f"{int(col['offset']):X}"
        w   = _parse_width(col["width"])
        spec = f"{name}:{off}:{w}"
        if "byte_slice" in col and col["byte_slice"] is not None:
            lo, hi = col["byte_slice"]
            spec += f":{int(lo)}:{int(hi)}"
        parts.append(spec)
    parts.append(wsl_path(csv_path))
    ack = await _send_and_wait(" ".join(parts),
                               ["ok poke_playback_start", "error poke_playback_start", "error unknown command"],
                               timeout=30)
    if ack and ack.startswith("ok poke_playback_start"):
        _break_sources.add("poke_trigger")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_playback_stop() -> str:
    """Stop CSV playback and remove its trigger."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("poke_playback_stop",
                               ["ok poke_playback_stop", "error unknown command"], timeout=5)
    if ack and ack.startswith("ok"):
        # Conservative: only safe when playback was the sole trigger. Users with
        # remaining static triggers should re-arm via poke_breakpoint_set.
        _break_sources.discard("poke_trigger")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def poke_playback_status() -> str:
    """Report CSV playback state: row cursor, triggers_seen, pokes, done flag.
    Use triggers_seen to validate the trigger PC — if it's zero after
    running for a while, the PC was never reached (wrong address)."""
    if not _alive():
        return "FAIL: No session"
    # "poke_playback" alone is too loose — it also matches stale acks like
    # "error unknown command: poke_playback_start". Anchor on the two legitimate
    # status prefixes plus the unknown-command fallthrough.
    ack = await _send_and_wait("poke_playback_status",
                               ["poke_playback pc=", "poke_playback inactive", "error unknown command"],
                               timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def step(count: int = 1) -> str:
    """Execute N CPU instructions (step into). Default 1."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"step {count}",
                               ["done step", "hit watchpoint", "hit read_watchpoint", "hit exception"],
                               timeout=30)
    if ack and "hit exception" in ack:
        return f"EXCEPTION: {ack}"
    return ack if ack else "FAIL: step timed out"


# ---------------------------------------------------------------------------
# Watchpoints
# ---------------------------------------------------------------------------

@mcp.tool()
async def watchpoint_set(address: str, value: str = "", log: bool = False) -> str:
    """Watch for 4-byte writes to address. Pauses on hit with full context.
    Optional: value="0x06037F20" only fires when the new value equals that.
    Optional: log=True logs full context (registers + call stack) to
    watchpoint_hits.txt without pausing. Use for surveying all writers."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    wp_file = _ipc_path("watchpoint_hits.txt")
    if os.path.exists(wp_file):
        os.remove(wp_file)
    cmd = f"watchpoint {addr}"
    if value:
        cmd += f" eq {_strip_hex(value)}"
    if log:
        cmd += " log"
    ack = await _send_and_wait(cmd, "ok watchpoint", timeout=5)
    if ack and not log:
        _break_sources.add("watchpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def watchpoint_clear() -> str:
    """Remove all watchpoints."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("watchpoint_clear", "ok watchpoint_clear", timeout=5)
    if ack:
        _break_sources.discard("watchpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def watchpoint_hits() -> str:
    """Read watchpoint hits since last watchpoint_set. Summarizes writer PCs."""
    if not _alive():
        return "FAIL: No session"
    wp_file = _ipc_path("watchpoint_hits.txt")
    if not os.path.exists(wp_file):
        return "No watchpoint hits recorded."
    with open(wp_file) as f:
        lines = f.readlines()
    hits = [l.strip() for l in lines if "pc=" in l]
    if not hits:
        return "No watchpoint hits recorded."
    pcs = {}
    samples = []
    for line in hits:
        for token in line.split():
            if token.startswith("pc="):
                pc = token.split("=", 1)[1]
                pcs[pc] = pcs.get(pc, 0) + 1
        if len(samples) < 20:
            samples.append(line)
    summary = f"Total hits: {len(hits)}\n\nWriter PCs:\n"
    for pc, count in sorted(pcs.items(), key=lambda x: -x[1]):
        summary += f"  {pc}: {count} writes\n"
    summary += f"\nFirst {min(len(samples), 20)} hits:\n"
    for line in samples:
        summary += f"  {line}\n"
    return summary


@mcp.tool()
async def read_watchpoint_set(address: str, log: bool = False) -> str:
    """Watch for reads from address. Pauses on hit with full context (PC, PR,
    registers, call stack) — same behavior as write watchpoints. Also logs all
    hits to read_watchpoint_hits.txt. Use to find which functions consume a field.
    frame_advance will return early with 'hit read_watchpoint' when triggered.
    Optional: log=True logs full context without pausing. Use for surveying
    all consumers over N frames."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    rwp_file = _ipc_path("read_watchpoint_hits.txt")
    if os.path.exists(rwp_file):
        os.remove(rwp_file)
    cmd = f"read_watchpoint {addr}"
    if log:
        cmd += " log"
    ack = await _send_and_wait(cmd, "ok read_watchpoint", timeout=5)
    if ack and not log:
        _break_sources.add("read_watchpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def read_watchpoint_clear() -> str:
    """Clear read watchpoint and resume if paused on a read watchpoint hit."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("read_watchpoint_clear", "ok read_watchpoint_clear", timeout=5)
    if ack:
        _break_sources.discard("read_watchpoint")
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def read_watchpoint_hits() -> str:
    """Read the accumulated hit log from read_watchpoint_set. Shows all reader PCs."""
    if not _alive():
        return "FAIL: No session"
    rwp_file = _ipc_path("read_watchpoint_hits.txt")
    if not os.path.exists(rwp_file):
        return "No read watchpoint hits recorded."
    with open(rwp_file) as f:
        lines = f.readlines()
    hits = [l.strip() for l in lines if "pc=" in l]
    if not hits:
        return "No read watchpoint hits recorded."
    pcs = {}
    samples = []
    for line in hits:
        for token in line.split():
            if token.startswith("pc="):
                pc = token.split("=", 1)[1]
                pcs[pc] = pcs.get(pc, 0) + 1
        if len(samples) < 20:
            samples.append(line)
    summary = f"Total read hits: {len(hits)}\n\nReader PCs:\n"
    for pc, count in sorted(pcs.items(), key=lambda x: -x[1]):
        summary += f"  {pc}: {count} reads\n"
    summary += f"\nFirst {min(len(samples), 20)} hits:\n"
    for line in samples:
        summary += f"  {line}\n"
    return summary


# ---------------------------------------------------------------------------
# Exception handling
# ---------------------------------------------------------------------------

@mcp.tool()
async def exception_break(mode: str = "enable") -> str:
    """Control SH-2 exception reporting. Modes:
    'enable' = pause on exception with full context (like breakpoints).
    'log' = log exceptions to exception_hits.txt without pausing.
    'disable' = default, let BIOS handle exceptions silently.
    Catches: address errors, illegal instructions, slot illegal, NMI."""
    if not _alive():
        return "FAIL: No session"
    if mode not in ("enable", "log", "disable"):
        return "FAIL: mode must be enable, log, or disable"
    ack = await _send_and_wait(f"exception_break {mode}", "ok exception_break", timeout=5)
    if ack:
        if mode == "enable":
            _break_sources.add("exception")
        else:
            _break_sources.discard("exception")
    return ack if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# Tracing
# ---------------------------------------------------------------------------

@mcp.tool()
async def call_trace_start() -> str:
    """Start recording function calls (JSR/BSR/BSRF) to a log file."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("call_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"call_trace {wsl_path(path)}", "ok call_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def call_trace_stop() -> str:
    """Stop call trace and return summary of targets called."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("call_trace_stop", "ok call_trace_stop", timeout=5)
    path = _ipc_path("call_trace.txt")
    if not os.path.exists(path):
        return "OK: trace stopped (no data)"
    with open(path) as f:
        lines = f.readlines()
    targets = {}
    for line in lines:
        parts = line.strip().split()
        if len(parts) >= 3:
            target = parts[-1]
            targets[target] = targets.get(target, 0) + 1
    summary = f"Total calls: {len(lines)}\n\nTop targets:\n"
    for target, count in sorted(targets.items(), key=lambda x: -x[1])[:30]:
        summary += f"  {target}: {count}\n"
    return summary


@mcp.tool()
async def pc_trace_frame() -> str:
    """Record every primary SH-2 PC for 1 frame. Returns path to binary file."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("pc_trace.bin")
    ack = await _send_and_wait(f"pc_trace_frame {wsl_path(path)}", "done pc_trace", timeout=30)
    if ack and os.path.exists(path):
        size = os.path.getsize(path)
        return f"OK: {size // 4} PCs recorded to {path} ({size} bytes)"
    return "FAIL: pc_trace timed out"


@mcp.tool()
async def unified_trace_start() -> str:
    """Start unified trace (call trace + CD block events interleaved)."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("unified_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"unified_trace {wsl_path(path)}", "ok unified_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def unified_trace_stop() -> str:
    """Stop unified trace."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("unified_trace_stop", "ok unified_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_trace_start() -> str:
    """Start logging button events per frame."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("input_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"input_trace {wsl_path(path)}", "ok input_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_trace_stop() -> str:
    """Stop input trace."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("input_trace_stop", "ok input_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_playback_start(path: str) -> str:
    """Start replaying a recorded input trace file. Events are injected at
    the correct frame numbers relative to the current frame. Works alongside
    any other command (sample_memory, run_to_frame, run_free, etc.).

    Load your save state FIRST, then start playback, then run your command.
    The playback auto-stops when all events are consumed.
    """
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(
        f"input_playback {wsl_path(path)}", "ok input_playback", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_playback_stop() -> str:
    """Stop input playback and clear all playback state."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("input_playback_stop", "ok input_playback_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def scdq_trace_start() -> str:
    """Start CD subsystem event trace."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("scdq_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"scdq_trace {wsl_path(path)}", "ok scdq_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def scdq_trace_stop() -> str:
    """Stop CD subsystem trace."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("scdq_trace_stop", "ok scdq_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdb_trace_start() -> str:
    """Start CD block trace."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("cdb_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"cdb_trace {wsl_path(path)}", "ok cdb_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdb_trace_stop() -> str:
    """Stop CD block trace."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("cdb_trace_stop", "ok cdb_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def insn_trace_start(start_line: int, stop_line: int) -> str:
    """Start per-instruction trace between line numbers."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("insn_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(
        f"insn_trace {wsl_path(path)} {start_line} {stop_line}",
        "ok insn_trace", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def insn_trace_unified_start(start_line: int, stop_line: int) -> str:
    """Start per-instruction trace into the unified trace file.
    Requires unified_trace_start() to be active first.
    """
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(
        f"insn_trace_unified {start_line} {stop_line}",
        "ok insn_trace_unified", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def insn_trace_stop() -> str:
    """Stop instruction trace (both standalone and unified modes)."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("insn_trace_stop", "insn_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# Save / Load state
# ---------------------------------------------------------------------------

@mcp.tool()
async def save_state(path: str) -> str:
    """Save emulator state to file (gzip, GUI-compatible). Path = Windows path."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"save_state {wsl_path(path)}", "ok save_state", timeout=10)
    if not ack:
        return "FAIL: save_state timed out"
    if "error" in ack:
        return f"FAIL: {ack}"
    return f"OK: State saved to {path}"


@mcp.tool()
async def load_state(path: str) -> str:
    """Load emulator state from file."""
    global _frame
    if not _alive():
        return "FAIL: No session"
    if not os.path.exists(path):
        return f"FAIL: File not found: {path}"
    ack = await _send_and_wait(f"load_state {wsl_path(path)}", "ok load_state", timeout=15)
    if not ack:
        return "FAIL: load_state timed out"
    if "error" in ack:
        return f"FAIL: {ack}"
    _frame = 0  # Reset — all frame references are relative to save state load
    return f"OK: State loaded from {path}"


# ---------------------------------------------------------------------------
# Region dumps
# ---------------------------------------------------------------------------

@mcp.tool()
async def dump_region(region: str, path: str) -> str:
    """Dump a full memory region to binary file.
    Regions: wram_high, wram_low, vdp1_vram, vdp2_vram, vdp2_cram, sound_ram.
    """
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"dump_region {region} {wsl_path(path)}", "dump_region", timeout=15)
    if ack and os.path.exists(path):
        size = os.path.getsize(path)
        return f"OK: {region} dumped to {path} ({size} bytes)"
    return "FAIL: dump_region timed out"


@mcp.tool()
async def dump_vdp2_regs(path: str) -> str:
    """Dump VDP2 register state to binary file."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"dump_vdp2_regs {wsl_path(path)}", "dump_vdp2_regs", timeout=5)
    return f"OK: VDP2 regs dumped to {path}" if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# CDL (Code/Data Logging)
# ---------------------------------------------------------------------------

@mcp.tool()
async def cdl_start(address_lo: str = "0x06000000", address_hi: str = "0x06100000") -> str:
    """Start code/data logging for an address range.
    Tracks which bytes are executed (CODE), read (DATA_READ), or written (DATA_WRITE).
    Defaults to High Work RAM (0x06000000-0x06100000, 1MB).
    Example for Low Work RAM: address_lo='0x00200000', address_hi='0x00300000'."""
    if not _alive():
        return "FAIL: No session"
    lo = _strip_hex(address_lo)
    hi = _strip_hex(address_hi)
    ack = await _send_and_wait(f"cdl_start {lo} {hi}", "cdl_start", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_stop() -> str:
    """Stop code/data logging (preserves bitmap)."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("cdl_stop", "cdl_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_reset() -> str:
    """Clear CDL bitmap without stopping logging."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("cdl_reset", "cdl_reset", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_dump(path: str) -> str:
    """Dump CDL bitmap (1MB, 1 byte per address) to file."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"cdl_dump {wsl_path(path)}", "cdl_dump", timeout=10)
    return f"OK: CDL dumped to {path}" if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_status() -> str:
    """Check if CDL is currently active."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("cdl_status", "cdl_status", timeout=5)
    return ack if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# DMA & memory profiling
# ---------------------------------------------------------------------------

@mcp.tool()
async def dma_trace_start() -> str:
    """Start logging all SCU DMA transfers."""
    if not _alive():
        return "FAIL: No session"
    path = _ipc_path("dma_trace.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"dma_trace {wsl_path(path)}", "ok dma_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def dma_trace_stop() -> str:
    """Stop DMA trace logging."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("dma_trace_stop", "ok dma_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def mem_profile_start(address_lo: str, address_hi: str) -> str:
    """Start logging all CPU writes in an address range. Addresses in hex."""
    if not _alive():
        return "FAIL: No session"
    lo = _strip_hex(address_lo)
    hi = _strip_hex(address_hi)
    path = _ipc_path("mem_profile.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(f"mem_profile {lo} {hi} {wsl_path(path)}", "ok mem_profile", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def mem_profile_stop() -> str:
    """Stop memory write profiling and return summary."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("mem_profile_stop", "ok mem_profile_stop", timeout=5)
    path = _ipc_path("mem_profile.txt")
    if not os.path.exists(path):
        return "OK: profiling stopped (no data)"
    with open(path) as f:
        lines = [l for l in f.readlines() if not l.startswith("#")]
    writers = {}
    for line in lines:
        for token in line.strip().split():
            if token.startswith("pc="):
                pc = token.split("=")[1]
                writers[pc] = writers.get(pc, 0) + 1
    summary = f"Total writes: {len(lines)}\n\nWriters:\n"
    for pc, count in sorted(writers.items(), key=lambda x: -x[1])[:20]:
        summary += f"  {pc}: {count}\n"
    return summary


@mcp.tool()
async def mem_read_profile_start(address_lo: str, address_hi: str) -> str:
    """Start logging all CPU reads in an address range. Addresses in hex.
    Logs pc, pr (caller), addr, and size for each read. Use to find all
    functions that read from a memory region (e.g. disc-loaded track data)."""
    if not _alive():
        return "FAIL: No session"
    lo = _strip_hex(address_lo)
    hi = _strip_hex(address_hi)
    path = _ipc_path("mem_read_profile.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(
        f"mem_read_profile {lo} {hi} {wsl_path(path)}",
        "ok mem_read_profile", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def mem_read_profile_stop() -> str:
    """Stop memory read profiling and return summary of reader PCs."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("mem_read_profile_stop", "ok mem_read_profile_stop", timeout=5)
    path = _ipc_path("mem_read_profile.txt")
    if not os.path.exists(path):
        return "OK: profiling stopped (no data)"
    with open(path) as f:
        lines = [l for l in f.readlines() if not l.startswith("#")]
    readers = {}
    for line in lines:
        for token in line.strip().split():
            if token.startswith("pc="):
                pc = token.split("=")[1]
                readers[pc] = readers.get(pc, 0) + 1
    summary = f"Total reads: {len(lines)}\n\nReaders:\n"
    for pc, count in sorted(readers.items(), key=lambda x: -x[1])[:20]:
        summary += f"  {pc}: {count}\n"
    return summary


# ---------------------------------------------------------------------------
# Execution control
# ---------------------------------------------------------------------------

@mcp.tool()
async def run_to_frame(frame: int) -> str:
    """Free-run until reaching frame N, then pause. If a watchpoint fires
    before reaching the target frame, stops early and reports the hit."""
    global _frame
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"run_to_frame {frame}",
                               ["done run_to_frame", "hit watchpoint", "hit read_watchpoint", "hit exception"],
                               timeout=300)
    if ack:
        import re
        m = re.search(r"frame=(\d+)", ack)
        if m:
            _frame = int(m.group(1))
        if "STOPPED_BY_WATCHPOINT" in ack or "hit watchpoint" in ack or "hit read_watchpoint" in ack:
            return f"STOPPED at frame {_frame} by watchpoint (target was {frame}):\n{ack}"
        if "STOPPED_BY_EXCEPTION" in ack or "hit exception" in ack:
            return f"EXCEPTION at frame {_frame} (target was {frame}):\n{ack}"
        _frame = frame
        return f"OK: At frame {frame}"
    return "FAIL: run_to_frame timed out"


@mcp.tool()
async def run_free(wait_for_break: bool | None = None, timeout: int = 300) -> str:
    """Unpause emulator (free-run). Automatically waits for a break event if
    any breakpoints, watchpoints, or exception breaks are active. Pass
    wait_for_break=False to override (e.g. letting the human play), or
    wait_for_break=True to force waiting (e.g. after raw_command breakpoints)."""
    if not _alive():
        return "FAIL: No session"
    _send("run")  # No ack from C++ -- single-threaded, guaranteed to execute
    should_wait = wait_for_break if wait_for_break is not None else bool(_break_sources)
    if not should_wait:
        return "ok run"
    ack = await _wait_ack(["break ", "hit watchpoint", "hit read_watchpoint", "hit exception", "done poke_playback"], timeout=timeout)
    if ack and "hit exception" in ack:
        return f"EXCEPTION: {ack}"
    return ack if ack else f"TIMEOUT: no break event within {timeout}s"


@mcp.tool()
async def pause() -> str:
    """Pause emulation."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("pause", "ok pause", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def deterministic() -> str:
    """Set fixed RTC seed for reproducible runs."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("deterministic", "deterministic", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def dump_cycle() -> str:
    """Get current CPU cycle count."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("dump_cycle", "ok dump_cycle", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def run_to_cycle(cycle: int) -> str:
    """Run until reaching a specific CPU cycle count. Stops early if a watchpoint fires."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"run_to_cycle {cycle}",
                               ["done run_to_cycle", "hit watchpoint", "hit read_watchpoint", "hit exception"],
                               timeout=60)
    if ack:
        if "STOPPED_BY_WATCHPOINT" in ack or "hit watchpoint" in ack or "hit read_watchpoint" in ack:
            return f"STOPPED by watchpoint (target cycle was {cycle}):\n{ack}"
        if "STOPPED_BY_EXCEPTION" in ack or "hit exception" in ack:
            return f"EXCEPTION (target cycle was {cycle}):\n{ack}"
        return ack
    return "FAIL: timed out"


# ---------------------------------------------------------------------------
# Window visibility
# ---------------------------------------------------------------------------

@mcp.tool()
async def show_window() -> str:
    """Make the emulator window visible."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("show_window", "ok show_window", timeout=5)
    return "OK: Window shown" if ack else "FAIL: timed out"


@mcp.tool()
async def hide_window() -> str:
    """Hide the emulator window."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("hide_window", "ok hide_window", timeout=5)
    return "OK: Window hidden" if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# VDP2 watchpoints
# ---------------------------------------------------------------------------

@mcp.tool()
async def vdp2_watchpoint_set(address_lo: str, address_hi: str) -> str:
    """Watch for VDP2 register writes in an address range. Addresses in hex."""
    if not _alive():
        return "FAIL: No session"
    lo = _strip_hex(address_lo)
    hi = _strip_hex(address_hi)
    path = _ipc_path("vdp2_watchpoint.txt")
    if os.path.exists(path):
        os.remove(path)
    ack = await _send_and_wait(
        f"vdp2_watchpoint {lo} {hi} {wsl_path(path)}",
        "ok vdp2_watchpoint", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def vdp2_watchpoint_clear() -> str:
    """Remove VDP2 watchpoint."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("vdp2_watchpoint_clear", "vdp2_watchpoint_clear", timeout=5)
    return ack if ack else "FAIL: timed out"


# ---------------------------------------------------------------------------
# Memory search (cheat-engine workflow)
# ---------------------------------------------------------------------------

@mcp.tool()
async def memory_snapshot(name: str = "baseline") -> str:
    """Snapshot all of WRAM High (1MB) for later comparison.
    This is the 'Set Original to Current' step in cheat-engine workflow.
    """
    if not _alive():
        return "FAIL: No session"
    dump_path = _ipc_path(f"snapshot_{name}.bin")
    ack = await _send_and_wait(
        f"dump_region wram_high {wsl_path(dump_path)}", "dump_region", timeout=15,
    )
    if not ack:
        return "FAIL: dump_region timed out"
    await asyncio.sleep(0.3)
    if not os.path.exists(dump_path):
        return "FAIL: snapshot file not created"
    with open(dump_path, "rb") as f:
        data = f.read()
    _memory_snapshots[name] = data
    return f"OK: Snapshot '{name}' saved ({len(data)} bytes, WRAM High 0x06000000-0x060FFFFF)"


@mcp.tool()
async def memory_search_exact(value: int, width: int = 32, snapshot_name: str = "baseline") -> str:
    """Search a memory snapshot for an exact value (big-endian SH-2).

    Args:
        value: The value to search for.
        width: Bit width (8, 16, or 32).
        snapshot_name: Which snapshot to search.
    Returns up to 50 matching addresses.
    """
    data = _memory_snapshots.get(snapshot_name)
    if data is None:
        return f"FAIL: No snapshot '{snapshot_name}'. Call memory_snapshot() first."
    fmt = {8: ">B", 16: ">H", 32: ">I"}
    if width not in fmt:
        return "FAIL: width must be 8, 16, or 32"
    step = width // 8
    target = struct.pack(fmt[width], value & ((1 << width) - 1))
    matches = []
    base = 0x06000000
    for i in range(0, len(data) - step + 1, step):
        if data[i:i + step] == target:
            matches.append(base + i)
            if len(matches) >= 50:
                break
    if not matches:
        return f"No matches for value {value} (0x{value & ((1 << width) - 1):X}) in '{snapshot_name}'"
    lines = [f"Found {len(matches)} matches for {value} (0x{value & ((1 << width) - 1):X}):"]
    for addr in matches:
        lines.append(f"  0x{addr:08X}")
    if len(matches) == 50:
        lines.append("  (truncated at 50)")
    return "\n".join(lines)


@mcp.tool()
async def memory_compare(old_snapshot: str = "baseline", new_snapshot: str = "current",
                    mode: str = "changed", width: int = 32) -> str:
    """Compare two memory snapshots to find changed values.

    Args:
        old_snapshot: Name of the 'before' snapshot.
        new_snapshot: Name of the 'after' snapshot. Use 'live' to take a fresh dump.
        mode: 'changed', 'increased', 'decreased', or 'unchanged'.
        width: Comparison width (8, 16, or 32).
    Returns up to 50 matching addresses.
    """
    old_data = _memory_snapshots.get(old_snapshot)
    if old_data is None:
        return f"FAIL: No snapshot '{old_snapshot}'. Call memory_snapshot() first."
    if new_snapshot == "live":
        result = await memory_snapshot("_live_compare")
        if result.startswith("FAIL"):
            return result
        new_data = _memory_snapshots["_live_compare"]
    else:
        new_data = _memory_snapshots.get(new_snapshot)
        if new_data is None:
            return f"FAIL: No snapshot '{new_snapshot}'."
    if len(old_data) != len(new_data):
        return f"FAIL: Snapshot size mismatch ({len(old_data)} vs {len(new_data)})"
    if width not in (8, 16, 32):
        return "FAIL: width must be 8, 16, or 32"
    step = width // 8
    fmt = {8: ">B", 16: ">H", 32: ">I"}[width]
    matches = []
    base = 0x06000000
    for i in range(0, min(len(old_data), len(new_data)) - step + 1, step):
        old_val = struct.unpack(fmt, old_data[i:i + step])[0]
        new_val = struct.unpack(fmt, new_data[i:i + step])[0]
        hit = False
        if mode == "changed" and old_val != new_val:
            hit = True
        elif mode == "increased" and new_val > old_val:
            hit = True
        elif mode == "decreased" and new_val < old_val:
            hit = True
        elif mode == "unchanged" and old_val == new_val:
            hit = True
        if hit:
            matches.append((base + i, old_val, new_val))
            if len(matches) >= 50:
                break
    if not matches:
        return f"No addresses matched filter '{mode}' between '{old_snapshot}' and '{new_snapshot}'"
    w = width // 4
    lines = [f"Found {len(matches)} matches (filter: {mode}, {width}-bit):"]
    for addr, old_v, new_v in matches:
        lines.append(f"  0x{addr:08X}: 0x{old_v:0{w}X} -> 0x{new_v:0{w}X} (delta={new_v - old_v:+d})")
    if len(matches) == 50:
        lines.append("  (truncated at 50)")
    return "\n".join(lines)


@mcp.tool()
async def memory_filter_candidates(candidates: str, mode: str = "changed", width: int = 32) -> str:
    """Narrow down candidate addresses by reading current values and comparing.

    Args:
        candidates: Comma-separated hex addresses (e.g. '0x06078900,0x0607890C').
        mode: 'changed', 'increased', 'decreased', 'unchanged'.
        width: Bit width (8, 16, or 32).
    """
    if not _alive():
        return "FAIL: No session"
    addrs = []
    for addr_str in candidates.split(","):
        addr_str = _strip_hex(addr_str.strip())
        if addr_str:
            addrs.append(int(addr_str, 16))
    if not addrs:
        return "FAIL: No candidate addresses provided"
    step = width // 8
    fmt = {8: ">B", 16: ">H", 32: ">I"}[width]
    values = {}
    for addr in addrs:
        out_path = _ipc_path("filter_read.bin")
        ack = await _send_and_wait(
            f"dump_mem_bin {addr:08X} {step:x} {wsl_path(out_path)}",
            "dump_mem_bin", timeout=5,
        )
        if ack:
            await asyncio.sleep(0.1)
            if os.path.exists(out_path):
                with open(out_path, "rb") as f:
                    d = f.read(step)
                if len(d) == step:
                    values[addr] = struct.unpack(fmt, d)[0]
    w = width // 4
    lines = [f"Read {len(values)} addresses:"]
    for addr, val in values.items():
        lines.append(f"  0x{addr:08X} = 0x{val:0{w}X}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# High-level workflows
# ---------------------------------------------------------------------------

# BIOS skip sequence — press START at frame 146 to skip the intro
_BIOS_SKIP = [
    (146, "input START"),
    (152, "input_release START"),
]


@mcp.tool()
async def capture_trace(
    cue_path: str = "",
    frames: int = 1500,
    output_path: str = "",
    skip_bios: bool = True,
    dma: bool = False,
    mem_profile_lo: str = "",
    mem_profile_hi: str = "",
    insn_after_event: int = 0,
    insn_until_event: int = 0,
) -> str:
    """Boot a disc, capture a unified trace for N frames, then quit.

    All-in-one trace capture. Boots Mednafen, records a unified trace
    (SH-2 function calls + CD block I/O), advances the specified number
    of frames, stops everything, quits, and returns a summary.

    The unified trace always runs. Each line is one EVENT — a function
    call (JSR/BSR) or a CD block state change. Events are numbered
    starting at line 3 (lines 1-2 are the header).

    Optional layers (all off by default):

      dma=True              Also log SCU DMA transfers (separate file).

      mem_profile_lo/hi     Also log CPU writes in an address range
                            (hex, e.g. "0x06028000"/"0x060FFFFF").
                            Both must be set. Separate file.

      insn_after_event +    Dump EVERY SH-2 instruction (with full
      insn_until_event      disassembly + all registers) between two
                            unified trace event numbers. For example,
                            insn_after_event=100, insn_until_event=105
                            logs all instructions that execute between
                            the 100th and 105th function-call/CD events.
                            WARNING: even a small event range produces
                            millions of instruction lines. Use sparingly.
                            Injected directly into the unified trace file.

    Output files (all in the same directory as output_path):
      capture_trace.txt     Unified trace (always).
      dma_trace.txt         DMA log (only if dma=True).
      mem_profile.txt       Memory write log (only if mem_profile set).

    Returns one summary string with file sizes, or a FAIL message.
    """
    # --- Boot ---
    boot_result = await boot(cue_path=cue_path, timeout=45, sound=False)
    if not boot_result.startswith("OK"):
        return f"FAIL: Boot failed — {boot_result}"

    # --- Resolve output paths ---
    out = output_path or _ipc_path("capture_trace.txt")
    out_abs = os.path.abspath(out)
    out_dir = os.path.dirname(out_abs)
    os.makedirs(out_dir, exist_ok=True)
    if os.path.exists(out_abs):
        os.remove(out_abs)

    dma_path = os.path.join(out_dir, "dma_trace.txt")
    mem_path = os.path.join(out_dir, "mem_profile.txt")
    do_mem = bool(mem_profile_lo and mem_profile_hi)
    do_insn = bool(insn_after_event and insn_until_event)

    # --- Start unified trace (always) ---
    ack = await _send_and_wait(
        f"unified_trace {wsl_path(out_abs)}", "ok unified_trace", timeout=10)
    if not ack:
        await quit_emulator()
        return "FAIL: unified_trace command timed out"

    # --- Start optional layers ---
    if dma:
        if os.path.exists(dma_path):
            os.remove(dma_path)
        await _send_and_wait(
            f"dma_trace {wsl_path(dma_path)}", "ok dma_trace", timeout=5)

    if do_mem:
        if os.path.exists(mem_path):
            os.remove(mem_path)
        lo = _strip_hex(mem_profile_lo)
        hi = _strip_hex(mem_profile_hi)
        await _send_and_wait(
            f"mem_profile {lo} {hi} {wsl_path(mem_path)}", "ok mem_profile", timeout=5)

    if do_insn:
        # Unified trace has a 2-line header; first real event is line 3.
        effective_start = max(insn_after_event, 3)
        await _send_and_wait(
            f"insn_trace_unified {effective_start} {insn_until_event}",
            "ok insn_trace_unified", timeout=5)

    # --- Advance with BIOS skip ---
    current_frame = 0
    if skip_bios:
        for target_frame, cmd in _BIOS_SKIP:
            if target_frame >= frames:
                break
            delta = target_frame - current_frame
            if delta > 0:
                fa_ack = await _send_and_wait(
                    f"frame_advance {delta}",
                    ["done frame_advance", "hit watchpoint"], timeout=180)
                if not fa_ack or not _alive():
                    await quit_emulator()
                    return f"FAIL: Died during frame advance (reached frame {current_frame} of {frames})"
                current_frame = target_frame
            await _send_and_wait(cmd, "ok", timeout=5)

    remaining = frames - current_frame
    if remaining > 0:
        fa_ack = await _send_and_wait(
            f"frame_advance {remaining}",
            ["done frame_advance", "hit watchpoint"], timeout=600)
        if not fa_ack or not _alive():
            await quit_emulator()
            return f"FAIL: Died during frame advance (reached frame {current_frame} of {frames})"

    # --- Stop everything and quit ---
    if do_insn:
        await _send_and_wait("insn_trace_stop", "insn_trace_stop", timeout=5)
    if do_mem:
        await _send_and_wait("mem_profile_stop", "ok mem_profile_stop", timeout=5)
    if dma:
        await _send_and_wait("dma_trace_stop", "ok dma_trace_stop", timeout=5)
    await _send_and_wait("unified_trace_stop", "ok unified_trace_stop", timeout=10)
    await quit_emulator()

    # --- Report ---
    def _file_stats(path):
        if not os.path.exists(path):
            return None
        sz = os.path.getsize(path)
        n = 0
        with open(path, "r", errors="replace") as f:
            for _ in f:
                n += 1
        return (sz, n)

    stats = _file_stats(out_abs)
    if not stats:
        return f"FAIL: Trace file not found at {out_abs}"

    parts = [f"OK: Captured {frames} frames"]
    parts.append(f"  unified: {out_abs} ({stats[0]:,} bytes, {stats[1]:,} lines)")

    if dma:
        ds = _file_stats(dma_path)
        if ds:
            parts.append(f"  dma: {dma_path} ({ds[0]:,} bytes, {ds[1]:,} lines)")

    if do_mem:
        ms = _file_stats(mem_path)
        if ms:
            parts.append(f"  mem_profile: {mem_path} ({ms[0]:,} bytes, {ms[1]:,} lines)")

    if do_insn:
        parts.append(f"  insn_trace: all instructions between events {insn_after_event}-{insn_until_event} (in unified file)")

    return "\n".join(parts)


# ---------------------------------------------------------------------------
# Escape hatch
# ---------------------------------------------------------------------------

@mcp.tool()
async def raw_command(command: str) -> str:
    """Send a raw automation command to Mednafen. Returns the ack response.
    Example: raw_command('dump_mem 06078900 100')
    """
    if not _alive():
        return "FAIL: No session"
    cmd_word = command.strip().split()[0] if command.strip() else ""
    ack = await _send_and_wait(command, cmd_word, timeout=15)
    if ack:
        return ack
    await asyncio.sleep(1)
    try:
        with open(_ack_file) as f:
            return f.read().strip()
    except (IOError, FileNotFoundError):
        return "FAIL: no response"


# ---------------------------------------------------------------------------
# Per-frame memory sampler
# ---------------------------------------------------------------------------

@mcp.tool()
async def sample_memory(address: str, size: int = 256, frames: int = 300,
                        output_path: str = "") -> str:
    """Dump a memory region every frame for N frames to a binary file.
    Runs at full emulator speed (no IPC per frame). Output is raw binary:
    <size> bytes per frame, <frames> frames total. Use tools/blob_to_csv.py
    to convert to CSV."""
    if not _alive():
        return "FAIL: No session"
    addr = _strip_hex(address)
    if not output_path:
        output_path = _ipc_path("mem_sample.bin")
    ack = await _send_and_wait(
        f"mem_sample {addr} {size:x} {frames} {wsl_path(output_path)}",
        ["done mem_sample", "error"], timeout=frames * 2 + 60
    )
    if not ack:
        return "FAIL: mem_sample timed out"
    if "error" in ack:
        return f"FAIL: {ack}"
    file_size = os.path.getsize(output_path) if os.path.exists(output_path) else 0
    return f"{ack}\nFile: {output_path} ({file_size} bytes)"


@mcp.tool()
async def sample_memory_stop() -> str:
    """Abort an in-progress memory sample early."""
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait("mem_sample_stop", "mem_sample_stop", timeout=10)
    return ack if ack else "FAIL: no response"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--cue", default=None)
    parser.add_argument("--ipc-dir", default=None)
    parser.add_argument("--home-dir", default=None)
    args = parser.parse_args()

    global _default_cue, _ipc_dir, _home_dir
    if args.cue:
        _default_cue = args.cue
    if args.ipc_dir:
        _ipc_dir = args.ipc_dir
    if args.home_dir:
        _home_dir = args.home_dir

    mcp.run()


if __name__ == "__main__":
    main()
