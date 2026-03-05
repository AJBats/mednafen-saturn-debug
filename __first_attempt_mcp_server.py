#!/usr/bin/env python3
"""Mednafen Saturn MCP Server — debug bridge for Claude Code.

Wraps Mednafen's file-based automation IPC as MCP tools, letting AI
assistants directly control the emulator: read memory, set breakpoints,
take screenshots, advance frames, trace calls, etc.

Architecture:
    Claude Code  --(stdio/MCP)-->  this server  --(file IPC)-->  WSL Mednafen

This server is game-agnostic. Game-specific helpers (struct layouts, menu
navigation, etc.) should be provided by the consuming project.

Setup:
    pip install mcp

    # Register with Claude Code (project scope — add to your game project):
    claude mcp add --transport stdio -s project mednafen -- python path/to/mcp_server.py

    # Or user scope (available in all projects):
    claude mcp add --transport stdio -s user mednafen -- python path/to/mcp_server.py

    # With a specific disc image auto-loaded:
    claude mcp add --transport stdio -s project mednafen -- python path/to/mcp_server.py --cue "path/to/game.cue"

Environment:
    MEDNAFEN_PATH  — path to WSL Mednafen binary (default: auto-detect from repo)
    MEDNAFEN_CUE   — default disc image path (Windows path, converted to WSL internally)
"""

import os
import sys
import time
import asyncio
import subprocess
import tempfile
import base64
import struct
import argparse
import logging

from mcp.server.fastmcp import FastMCP

logger = logging.getLogger(__name__)

mcp = FastMCP("mednafen")

# ---------------------------------------------------------------------------
# Shotgun debug logger — writes to mcp_debug.log in cwd, flushed every line
# ---------------------------------------------------------------------------
_debug_log_path = os.path.join(os.getcwd(), "mcp_debug.log")

def MCPLOG(msg):
    with open(_debug_log_path, "a") as f:
        f.write(f"{time.time():.3f} {msg}\n")
        f.flush()

MCPLOG("MODULE LOADED")


# ---------------------------------------------------------------------------
# Diagnostic: trivial no-arg tool to test if no-arg async tools work
# ---------------------------------------------------------------------------

@mcp.tool()
async def ping() -> str:
    """Diagnostic: returns 'pong'. Tests if no-arg async tools work."""
    MCPLOG("ping: ENTER")
    return "pong"


# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def wsl_path(win_path):
    """Convert Windows path to WSL path."""
    if not win_path or ":" not in win_path[:3]:
        return win_path  # Already a WSL path or relative
    drive = win_path[0].lower()
    rest = win_path[2:].replace("\\", "/")
    return f"/mnt/{drive}{rest}"


def find_mednafen_binary():
    """Auto-detect Mednafen binary path from this repo."""
    # Check if we're inside the mednafen repo
    candidate = os.path.join(SCRIPT_DIR, "src", "mednafen")
    if os.path.exists(candidate):
        return candidate
    # Check environment
    env_path = os.environ.get("MEDNAFEN_PATH")
    if env_path:
        return env_path
    return None


# ---------------------------------------------------------------------------
# Configuration (set via CLI args or environment)
# ---------------------------------------------------------------------------

_config = {
    "mednafen_path": None,  # Windows path to mednafen binary
    "default_cue": None,    # Windows path to default disc image
    "ipc_base": None,       # Windows path to IPC directory
}


# ---------------------------------------------------------------------------
# Mednafen IPC session
# ---------------------------------------------------------------------------

class MednafenSession:
    """Manages a Mednafen emulator instance via file IPC."""

    def __init__(self, ipc_dir):
        self.ipc_dir = ipc_dir
        os.makedirs(self.ipc_dir, exist_ok=True)
        self.action_file = os.path.join(ipc_dir, "mednafen_action.txt")
        self.ack_file = os.path.join(ipc_dir, "mednafen_ack.txt")
        self.wp_file = os.path.join(ipc_dir, "watchpoint_hits.txt")
        self.seq = 0
        self.last_ack = ""
        self.proc = None
        self.stderr_file = None
        self.current_frame = 0

    async def start(self, cue_path, mednafen_path=None, timeout=45, extra_args=None):
        """Launch Mednafen in automation mode.

        Args:
            cue_path: Windows path to .cue disc image.
            mednafen_path: Windows path to mednafen binary (or WSL path).
            timeout: Seconds to wait for ready signal.
            extra_args: Additional CLI args for Mednafen.

        Returns:
            (success: bool, message: str)
        """
        if mednafen_path is None:
            mednafen_path = _config["mednafen_path"] or find_mednafen_binary()
        if mednafen_path is None:
            return False, "Cannot find Mednafen binary. Set MEDNAFEN_PATH or --mednafen-path."

        if not os.path.exists(cue_path):
            return False, f"Disc image not found: {cue_path}"

        cue_wsl = wsl_path(cue_path)
        mednafen_wsl = wsl_path(mednafen_path)
        ipc_wsl = wsl_path(self.ipc_dir)

        # Clean IPC files
        for f in [self.action_file, self.ack_file, self.wp_file]:
            if os.path.exists(f):
                os.remove(f)

        extra = " ".join(extra_args) if extra_args else ""
        # WSLg provides DISPLAY=:0 automatically on Windows 11
        launch_cmd = (
            f'export DISPLAY=:0; '
            f'rm -f "$HOME/.mednafen/mednafen.lck"; '
            f'"{mednafen_wsl}" '
            f'--sound 0 --automation "{ipc_wsl}" {extra} "{cue_wsl}"'
        )

        self.stderr_file = tempfile.NamedTemporaryFile(
            mode="w", suffix="_mednafen_stderr.txt", delete=False,
        )
        self.proc = subprocess.Popen(
            ["wsl", "-d", "Ubuntu", "-e", "bash", "-c", launch_cmd],
            stdout=subprocess.DEVNULL,
            stderr=self.stderr_file,
        )

        # Wait for ready (async — yields to event loop)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return False, f"Mednafen exited with code {self.proc.returncode}"
            if os.path.exists(self.ack_file):
                try:
                    with open(self.ack_file) as f:
                        content = f.read().strip()
                    if "ready" in content:
                        self.last_ack = content
                        self.current_frame = 0
                        return True, "Mednafen ready"
                except (IOError, PermissionError):
                    pass
            await asyncio.sleep(0.2)
        return False, "Timeout waiting for Mednafen to start"

    def send(self, cmd):
        """Send a command via action file."""
        self.seq += 1
        padding = "." * (self.seq % 16)
        tmp = self.action_file + ".tmp"
        with open(tmp, "w", newline="\n") as f:
            f.write(f"# {self.seq}{padding}\n")
            f.write(cmd + "\n")
        if os.path.exists(self.action_file):
            os.remove(self.action_file)
        os.rename(tmp, self.action_file)

    async def wait_ack(self, keyword, timeout=30):
        """Wait for ack containing keyword (async — yields to event loop)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc and self.proc.poll() is not None:
                return None
            if os.path.exists(self.ack_file):
                try:
                    with open(self.ack_file) as f:
                        content = f.read().strip()
                except (IOError, PermissionError):
                    await asyncio.sleep(0.05)
                    continue
                if content != self.last_ack and keyword in content:
                    self.last_ack = content
                    return content
            await asyncio.sleep(0.05)
        return None

    async def send_and_wait(self, cmd, keyword, timeout=30):
        """Send command and wait for matching ack."""
        self.send(cmd)
        return await self.wait_ack(keyword, timeout)

    async def frame_advance(self, n=1, timeout=180):
        """Advance N frames."""
        ack = await self.send_and_wait(
            f"frame_advance {n}", "done frame_advance", timeout=timeout
        )
        if ack:
            self.current_frame += n
        return ack

    def quit(self):
        """Shutdown Mednafen."""
        if self.proc and self.proc.poll() is None:
            self.send("quit")
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.stderr_file:
            self.stderr_file.close()
            try:
                os.unlink(self.stderr_file.name)
            except OSError:
                pass
        self.proc = None

    @property
    def is_alive(self):
        return self.proc is not None and self.proc.poll() is None


# ---------------------------------------------------------------------------
# Global session
# ---------------------------------------------------------------------------

session: MednafenSession | None = None


def _require_session():
    """Ensure a session is running. Returns (session, error_msg)."""
    if session is None or not session.is_alive:
        return None, "No Mednafen session running. Call boot() first."
    return session, None


# ===========================================================================
# MCP Tools: Lifecycle
# ===========================================================================

@mcp.tool()
async def boot(cue_path: str = "", timeout: int = 45) -> str:
    """Launch Mednafen with a disc image.

    Args:
        cue_path: Windows path to .cue file. If empty, uses --cue default.
        timeout: Seconds to wait for emulator ready (default 45).

    The emulator starts paused at frame 0. Use frame_advance() or
    input_press() to navigate to the desired game state.
    """
    MCPLOG("boot: ENTER")
    global session
    if session is not None and session.is_alive:
        session.quit()

    cue = cue_path or _config.get("default_cue") or os.environ.get("MEDNAFEN_CUE", "")
    if not cue:
        return "FAIL: No disc image specified. Pass cue_path or set MEDNAFEN_CUE."

    ipc_dir = _config.get("ipc_base") or os.path.join(tempfile.gettempdir(), "mednafen_mcp_ipc")
    session = MednafenSession(ipc_dir)
    ok, msg = await session.start(cue, timeout=timeout)
    if not ok:
        session = None
        return f"FAIL: {msg}"

    return f"OK: {msg}. Emulator paused at frame 0."


@mcp.tool()
async def quit_emulator() -> str:
    """Shutdown Mednafen."""
    global session
    if session is not None:
        session.quit()
        session = None
        return "OK: Mednafen stopped"
    return "OK: No session was running"


@mcp.tool()
async def status() -> str:
    """Get current emulator status (frame number, pause state, breakpoints)."""
    MCPLOG("status: ENTER")
    s, err = _require_session()
    MCPLOG(f"status: _require_session -> err={err}")
    if err:
        return err
    MCPLOG("status: calling send_and_wait")
    ack = await s.send_and_wait("status", "status", timeout=5)
    MCPLOG(f"status: ack={ack[:80] if ack else 'None'}")
    return f"Frame: {s.current_frame}\n{ack or ''}"


# ===========================================================================
# MCP Tools: Frame control & input
# ===========================================================================

@mcp.tool()
async def frame_advance(count: int = 1) -> str:
    """Advance the emulator by N frames (default 1). Emulator pauses after."""
    MCPLOG(f"frame_advance: ENTER count={count}")
    s, err = _require_session()
    if err:
        return err
    ack = await s.frame_advance(count)
    MCPLOG(f"frame_advance: ack={ack[:80] if ack else 'None'}")
    if ack:
        return f"OK: Advanced {count} frames. Now at frame {s.current_frame}"
    return "FAIL: frame_advance timed out"


@mcp.tool()
async def input_press(button: str) -> str:
    """Press and hold a button. Stays held until input_release.
    Buttons: START, A, B, C, X, Y, Z, UP, DOWN, LEFT, RIGHT, L, R.
    """
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(f"input {button}", "ok input", timeout=5)
    return f"OK: Pressed {button}" if ack else f"FAIL: input {button} timed out"


@mcp.tool()
async def input_release(button: str) -> str:
    """Release a held button."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(f"input_release {button}", "ok input_release", timeout=5)
    return f"OK: Released {button}" if ack else f"FAIL: input_release {button}"


@mcp.tool()
async def input_clear() -> str:
    """Release all held buttons."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("input_clear", "ok input_clear", timeout=5)
    return "OK: All buttons released" if ack else "FAIL: input_clear timed out"


@mcp.tool()
async def input_tap(button: str, hold_frames: int = 5) -> str:
    """Press a button for N frames then release. Convenience for menu navigation."""
    s, err = _require_session()
    if err:
        return err
    await s.send_and_wait(f"input {button}", "ok input", timeout=5)
    await s.frame_advance(hold_frames)
    await s.send_and_wait(f"input_release {button}", "ok input_release", timeout=5)
    return f"OK: Tapped {button} for {hold_frames} frames. Now at frame {s.current_frame}"


# ===========================================================================
# MCP Tools: Memory reading
# ===========================================================================

@mcp.tool()
async def read_memory(address: str, length: int = 64) -> str:
    """Read memory as hex dump text. Address in hex (e.g. '0x06078900'). Max 4096 bytes."""
    s, err = _require_session()
    if err:
        return err
    length = min(length, 4096)
    addr = address.replace("0x", "").replace("0X", "")
    ack = await s.send_and_wait(f"dump_mem {addr} {length:x}", "mem ", timeout=10)
    return ack if ack else "FAIL: dump_mem timed out"


@mcp.tool()
async def read_memory_binary(address: str, length: int = 256) -> str:
    """Read memory as formatted hex+ASCII dump. Max 65536 bytes."""
    s, err = _require_session()
    if err:
        return err
    length = min(length, 65536)
    addr = address.replace("0x", "").replace("0X", "")
    out_path = os.path.join(s.ipc_dir, "mem_dump.bin")
    out_wsl = wsl_path(out_path)

    ack = await s.send_and_wait(
        f"dump_mem_bin {addr} {length:x} {out_wsl}",
        "dump_mem_bin", timeout=10,
    )
    if not ack:
        return "FAIL: dump_mem_bin timed out"

    await asyncio.sleep(0.3)
    if not os.path.exists(out_path):
        return "FAIL: dump file not created"

    with open(out_path, "rb") as f:
        data = f.read()

    lines = []
    base_addr = int(addr, 16)
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {base_addr + i:08X}: {hex_part:<48s} {ascii_part}")

    return "\n".join(lines)


@mcp.tool()
async def read_u32(address: str) -> str:
    """Read a 32-bit big-endian value. Returns hex, unsigned, and signed decimal."""
    s, err = _require_session()
    if err:
        return err
    addr = address.replace("0x", "").replace("0X", "")
    out_path = os.path.join(s.ipc_dir, "u32_dump.bin")
    out_wsl = wsl_path(out_path)

    ack = await s.send_and_wait(f"dump_mem_bin {addr} 4 {out_wsl}", "dump_mem_bin", timeout=10)
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
    s, err = _require_session()
    if err:
        return err
    addr = address.replace("0x", "").replace("0X", "")
    out_path = os.path.join(s.ipc_dir, "u16_dump.bin")
    out_wsl = wsl_path(out_path)

    ack = await s.send_and_wait(f"dump_mem_bin {addr} 2 {out_wsl}", "dump_mem_bin", timeout=10)
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


# ===========================================================================
# MCP Tools: Registers
# ===========================================================================

@mcp.tool()
async def dump_regs() -> str:
    """Dump primary SH-2 registers (R0-R15, PC, SR, PR, GBR, VBR, MACH, MACL)."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("dump_regs", "R0=", timeout=10)
    return ack if ack else "FAIL: dump_regs timed out"


@mcp.tool()
async def dump_slave_regs() -> str:
    """Dump secondary SH-2 registers."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("dump_slave_regs", "R0=", timeout=10)
    return ack if ack else "FAIL: dump_slave_regs timed out"


@mcp.tool()
async def dump_regs_binary() -> str:
    """Dump primary SH-2 registers to binary file and return formatted output.
    22 registers as uint32: R0-R15, PC, SR, PR, GBR, VBR, MACH, MACL.
    """
    s, err = _require_session()
    if err:
        return err
    out_path = os.path.join(s.ipc_dir, "regs.bin")
    out_wsl = wsl_path(out_path)
    ack = await s.send_and_wait(f"dump_regs_bin {out_wsl}", "dump_regs_bin", timeout=10)
    if not ack:
        return "FAIL: timed out"

    await asyncio.sleep(0.2)
    if not os.path.exists(out_path):
        return "FAIL: file not created"

    with open(out_path, "rb") as f:
        data = f.read()

    if len(data) < 88:
        return f"FAIL: expected 88 bytes, got {len(data)}"

    # Little-endian (x86 host writes)
    regs = struct.unpack("<22I", data)
    names = [f"R{i}" for i in range(16)] + ["PC", "SR", "PR", "GBR", "VBR", "MACH"]
    lines = []
    for i, name in enumerate(names):
        lines.append(f"  {name:5s} = 0x{regs[i]:08X}")
    return "\n".join(lines)


# ===========================================================================
# MCP Tools: Breakpoints & stepping
# ===========================================================================

@mcp.tool()
async def breakpoint_set(address: str) -> str:
    """Set a PC breakpoint. Address in hex (e.g. '0x0600C5D6')."""
    s, err = _require_session()
    if err:
        return err
    addr = address.replace("0x", "").replace("0X", "")
    ack = await s.send_and_wait(f"breakpoint {addr}", "ok breakpoint", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_remove(address: str) -> str:
    """Remove a single breakpoint."""
    s, err = _require_session()
    if err:
        return err
    addr = address.replace("0x", "").replace("0X", "")
    ack = await s.send_and_wait(f"breakpoint_remove {addr}", "breakpoint_remove", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_clear() -> str:
    """Remove all breakpoints."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("breakpoint_clear", "breakpoint_clear", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def breakpoint_list() -> str:
    """List all active breakpoints."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("breakpoint_list", "breakpoints", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def continue_execution() -> str:
    """Resume execution until a breakpoint is hit (max 30s timeout)."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("continue", "break", timeout=30)
    return ack if ack else "FAIL: no breakpoint hit within 30s"


@mcp.tool()
async def step(count: int = 1) -> str:
    """Execute N CPU instructions (step into subroutines). Default 1."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(f"step {count}", "done step", timeout=30)
    return ack if ack else "FAIL: step timed out"


# ===========================================================================
# MCP Tools: Watchpoints
# ===========================================================================

@mcp.tool()
async def watchpoint_set(address: str) -> str:
    """Watch for 4-byte writes to address. Hits are logged non-blocking.
    Use watchpoint_hits() to read results.
    """
    s, err = _require_session()
    if err:
        return err
    addr = address.replace("0x", "").replace("0X", "")
    if os.path.exists(s.wp_file):
        os.remove(s.wp_file)
    ack = await s.send_and_wait(f"watchpoint {addr}", "ok watchpoint", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def watchpoint_clear() -> str:
    """Remove all watchpoints."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("watchpoint_clear", "ok watchpoint_clear", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def watchpoint_hits() -> str:
    """Read watchpoint hits since last watchpoint_set.
    Returns writer PCs, old/new values, and frame numbers.
    """
    s, err = _require_session()
    if err:
        return err
    if not os.path.exists(s.wp_file):
        return "No watchpoint hits recorded."

    with open(s.wp_file) as f:
        lines = f.readlines()

    hits = [l.strip() for l in lines if "pc=" in l]
    if not hits:
        return "No watchpoint hits recorded."

    pcs = {}
    samples = []
    for line in hits:
        parts = {}
        for token in line.split():
            if "=" in token:
                k, v = token.split("=", 1)
                parts[k] = v
        if "pc" in parts:
            pc = parts["pc"]
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


# ===========================================================================
# MCP Tools: Screenshot
# ===========================================================================

@mcp.tool()
async def screenshot(output_path: str = "") -> str:
    """Take a screenshot of the current frame.
    Returns the Windows path to the saved PNG file.

    Args:
        output_path: Windows path for the PNG. Default: mcp_screenshot.png in project root.
    """
    MCPLOG("screenshot: ENTER")
    s, err = _require_session()
    MCPLOG(f"screenshot: _require_session -> err={err}")
    if err:
        return err

    shot_path = output_path or os.path.join(os.getcwd(), "mcp_screenshot.png")
    MCPLOG(f"screenshot: shot_path={shot_path}")
    shot_wsl = wsl_path(shot_path)
    MCPLOG(f"screenshot: shot_wsl={shot_wsl}")

    if os.path.exists(shot_path):
        MCPLOG("screenshot: removing old file")
        os.remove(shot_path)

    MCPLOG("screenshot: calling send_and_wait")
    ack = await s.send_and_wait(f"screenshot {shot_wsl}", "ok screenshot", timeout=10)
    MCPLOG(f"screenshot: ack={ack}")
    if not ack:
        return "FAIL: screenshot timed out"

    if ack.startswith("error"):
        return f"FAIL: {ack}"

    size = os.path.getsize(shot_path) if os.path.exists(shot_path) else 0
    MCPLOG(f"screenshot: OK size={size}")
    return f"OK: Screenshot saved to {shot_path} ({size} bytes)"


# ===========================================================================
# MCP Tools: Tracing
# ===========================================================================

@mcp.tool()
async def call_trace_start() -> str:
    """Start recording function calls (JSR/BSR/BSRF) to a log file."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "call_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(f"call_trace {trace_wsl}", "ok call_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def call_trace_stop() -> str:
    """Stop call trace and return summary of targets called."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("call_trace_stop", "ok call_trace_stop", timeout=5)

    trace_path = os.path.join(s.ipc_dir, "call_trace.txt")
    if not os.path.exists(trace_path):
        return "OK: trace stopped (no data)"

    with open(trace_path) as f:
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
    """Record every primary SH-2 PC value for 1 frame.
    Returns path to binary file (sequence of uint32 PCs).
    """
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "pc_trace.bin")
    trace_wsl = wsl_path(trace_path)
    ack = await s.send_and_wait(f"pc_trace_frame {trace_wsl}", "done pc_trace", timeout=30)
    if ack and os.path.exists(trace_path):
        size = os.path.getsize(trace_path)
        count = size // 4
        return f"OK: {count} PCs recorded to {trace_path} ({size} bytes)"
    return "FAIL: pc_trace timed out"


# ===========================================================================
# MCP Tools: Save/Load state
# ===========================================================================

@mcp.tool()
async def save_state(path: str) -> str:
    """Save emulator state to file (gzip format, GUI-compatible).
    Path is a Windows path (e.g. 'D:/saves/race.mc0').
    """
    s, err = _require_session()
    if err:
        return err
    state_wsl = wsl_path(path)
    ack = await s.send_and_wait(f"save_state {state_wsl}", "save_state", timeout=10)
    return f"OK: State saved to {path}" if ack else "FAIL: timed out"


@mcp.tool()
async def load_state(path: str) -> str:
    """Load emulator state from file."""
    s, err = _require_session()
    if err:
        return err
    if not os.path.exists(path):
        return f"FAIL: File not found: {path}"
    state_wsl = wsl_path(path)
    ack = await s.send_and_wait(f"load_state {state_wsl}", "load_state", timeout=15)
    return f"OK: State loaded from {path}" if ack else "FAIL: timed out"


# ===========================================================================
# MCP Tools: Region dumps
# ===========================================================================

@mcp.tool()
async def dump_region(region: str, path: str) -> str:
    """Dump a full memory region to binary file.
    Regions: wram_high, wram_low, vdp1_vram, vdp2_vram, vdp2_cram, sound_ram.
    Path is a Windows path for the output file.
    """
    s, err = _require_session()
    if err:
        return err
    out_wsl = wsl_path(path)
    ack = await s.send_and_wait(f"dump_region {region} {out_wsl}", "dump_region", timeout=15)
    if ack and os.path.exists(path):
        size = os.path.getsize(path)
        return f"OK: {region} dumped to {path} ({size} bytes)"
    return "FAIL: dump_region timed out"


# ===========================================================================
# MCP Tools: CDL (Code/Data Logging)
# ===========================================================================

@mcp.tool()
async def cdl_start() -> str:
    """Start code/data logging. Tracks which bytes are executed vs read vs written."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("cdl_start", "cdl_start", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_stop() -> str:
    """Stop code/data logging (preserves bitmap)."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("cdl_stop", "cdl_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_reset() -> str:
    """Clear CDL bitmap without stopping logging."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("cdl_reset", "cdl_reset", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_dump(path: str) -> str:
    """Dump CDL bitmap (1MB, 1 byte per address) to file."""
    s, err = _require_session()
    if err:
        return err
    out_wsl = wsl_path(path)
    ack = await s.send_and_wait(f"cdl_dump {out_wsl}", "cdl_dump", timeout=10)
    return f"OK: CDL dumped to {path}" if ack else "FAIL: timed out"


@mcp.tool()
async def cdl_status() -> str:
    """Check if CDL is currently active."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("cdl_status", "cdl_status", timeout=5)
    return ack if ack else "FAIL: timed out"


# ===========================================================================
# MCP Tools: DMA & memory profiling
# ===========================================================================

@mcp.tool()
async def dma_trace_start() -> str:
    """Start logging all SCU DMA transfers."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "dma_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(f"dma_trace {trace_wsl}", "ok dma_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def dma_trace_stop() -> str:
    """Stop DMA trace logging."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("dma_trace_stop", "ok dma_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def mem_profile_start(address_lo: str, address_hi: str) -> str:
    """Start logging all CPU writes in an address range.
    Addresses in hex (e.g. '0x06078900', '0x06078B68').
    """
    s, err = _require_session()
    if err:
        return err
    lo = address_lo.replace("0x", "").replace("0X", "")
    hi = address_hi.replace("0x", "").replace("0X", "")
    profile_path = os.path.join(s.ipc_dir, "mem_profile.txt")
    profile_wsl = wsl_path(profile_path)
    if os.path.exists(profile_path):
        os.remove(profile_path)
    ack = await s.send_and_wait(
        f"mem_profile {lo} {hi} {profile_wsl}", "ok mem_profile", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def mem_profile_stop() -> str:
    """Stop memory write profiling and return summary."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("mem_profile_stop", "ok mem_profile_stop", timeout=5)

    profile_path = os.path.join(s.ipc_dir, "mem_profile.txt")
    if not os.path.exists(profile_path):
        return "OK: profiling stopped (no data)"

    with open(profile_path) as f:
        lines = f.readlines()

    # Summarize by writer PC
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


# ===========================================================================
# MCP Tools: Missing automation commands
# ===========================================================================

@mcp.tool()
async def run_to_frame(frame: int) -> str:
    """Free-run until reaching frame N, then pause."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(f"run_to_frame {frame}", "done run_to_frame", timeout=300)
    if ack:
        s.current_frame = frame
        return f"OK: At frame {frame}"
    return "FAIL: run_to_frame timed out"


@mcp.tool()
async def run_free() -> str:
    """Unpause emulator (free-run). Use pause() or frame_advance() to stop."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("run", "ok run", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def pause() -> str:
    """Pause emulation."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("pause", "ok pause", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def deterministic() -> str:
    """Set fixed RTC seed for reproducible runs."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("deterministic", "deterministic", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def show_window() -> str:
    """Make the emulator window visible."""
    s, err = _require_session()
    if err:
        return err
    s.send("show_window")
    return "OK: Window shown"


@mcp.tool()
async def hide_window() -> str:
    """Hide the emulator window."""
    s, err = _require_session()
    if err:
        return err
    s.send("hide_window")
    return "OK: Window hidden"


@mcp.tool()
async def dump_vdp2_regs(path: str) -> str:
    """Dump VDP2 register state to file."""
    s, err = _require_session()
    if err:
        return err
    out_wsl = wsl_path(path)
    ack = await s.send_and_wait(f"dump_vdp2_regs {out_wsl}", "dump_vdp2_regs", timeout=5)
    return f"OK: VDP2 regs dumped to {path}" if ack else "FAIL: timed out"


@mcp.tool()
async def dump_cycle() -> str:
    """Get current CPU cycle count."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("dump_cycle", "ok dump_cycle", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def run_to_cycle(cycle: int) -> str:
    """Run until reaching a specific CPU cycle count."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(f"run_to_cycle {cycle}", "done run_to_cycle", timeout=60)
    return ack if ack else "FAIL: timed out"


# ===========================================================================
# MCP Tools: VDP2 watchpoints
# ===========================================================================

@mcp.tool()
async def vdp2_watchpoint_set(address_lo: str, address_hi: str) -> str:
    """Watch for VDP2 register writes in an address range.
    Addresses in hex (e.g. '0x05F80000', '0x05F80120').
    """
    s, err = _require_session()
    if err:
        return err
    lo = address_lo.replace("0x", "").replace("0X", "")
    hi = address_hi.replace("0x", "").replace("0X", "")
    wp_path = os.path.join(s.ipc_dir, "vdp2_watchpoint.txt")
    wp_wsl = wsl_path(wp_path)
    if os.path.exists(wp_path):
        os.remove(wp_path)
    ack = await s.send_and_wait(
        f"vdp2_watchpoint {lo} {hi} {wp_wsl}",
        "ok vdp2_watchpoint", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def vdp2_watchpoint_clear() -> str:
    """Remove VDP2 watchpoint."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("vdp2_watchpoint_clear", "vdp2_watchpoint_clear", timeout=5)
    return ack if ack else "FAIL: timed out"


# ===========================================================================
# MCP Tools: Additional tracing
# ===========================================================================

@mcp.tool()
async def insn_trace_start(start_line: int, stop_line: int) -> str:
    """Start per-instruction trace between line numbers.
    Logs every instruction executed.
    """
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "insn_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(
        f"insn_trace {trace_wsl} {start_line} {stop_line}",
        "ok insn_trace", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def insn_trace_unified_start(start_line: int, stop_line: int) -> str:
    """Start per-instruction trace into the unified trace file (no separate path).

    Requires unified_trace_start() to be active first.
    """
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait(
        f"insn_trace_unified {start_line} {stop_line}",
        "ok insn_trace_unified", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def insn_trace_stop() -> str:
    """Stop instruction trace (both standalone and unified modes)."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("insn_trace_stop", "insn_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def unified_trace_start() -> str:
    """Start unified trace (call trace + CD block events interleaved)."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "unified_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(
        f"unified_trace {trace_wsl}", "ok unified_trace", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def unified_trace_stop() -> str:
    """Stop unified trace."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("unified_trace_stop", "ok unified_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_trace_start() -> str:
    """Start logging button events per frame."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "input_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(
        f"input_trace {trace_wsl}", "ok input_trace", timeout=5,
    )
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def input_trace_stop() -> str:
    """Stop input trace."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("input_trace_stop", "ok input_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def scdq_trace_start() -> str:
    """Start CD subsystem event trace."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "scdq_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(f"scdq_trace {trace_wsl}", "ok scdq_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def scdq_trace_stop() -> str:
    """Stop CD subsystem trace."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("scdq_trace_stop", "ok scdq_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdb_trace_start() -> str:
    """Start CD block trace."""
    s, err = _require_session()
    if err:
        return err
    trace_path = os.path.join(s.ipc_dir, "cdb_trace.txt")
    trace_wsl = wsl_path(trace_path)
    if os.path.exists(trace_path):
        os.remove(trace_path)
    ack = await s.send_and_wait(f"cdb_trace {trace_wsl}", "ok cdb_trace", timeout=5)
    return ack if ack else "FAIL: timed out"


@mcp.tool()
async def cdb_trace_stop() -> str:
    """Stop CD block trace."""
    s, err = _require_session()
    if err:
        return err
    ack = await s.send_and_wait("cdb_trace_stop", "ok cdb_trace_stop", timeout=5)
    return ack if ack else "FAIL: timed out"


# ===========================================================================
# MCP Tools: Memory search (cheat-engine style)
# ===========================================================================

_memory_snapshots: dict[str, bytes] = {}


@mcp.tool()
async def memory_snapshot(name: str = "baseline") -> str:
    """Snapshot all of WRAM High (1MB) for later comparison.
    This is the 'Set Original to Current' step in cheat-engine workflow.
    """
    s, err = _require_session()
    if err:
        return err
    dump_path = os.path.join(s.ipc_dir, f"snapshot_{name}.bin")
    dump_wsl = wsl_path(dump_path)

    ack = await s.send_and_wait(
        f"dump_region wram_high {dump_wsl}", "dump_region", timeout=15,
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
    """Search a memory snapshot for an exact value.

    Args:
        value: The value to search for.
        width: Bit width — 8, 16, or 32. Data is big-endian (SH-2).
        snapshot_name: Which snapshot to search (default 'baseline').

    Returns matching addresses (up to 50 results).
    """
    data = _memory_snapshots.get(snapshot_name)
    if data is None:
        return f"FAIL: No snapshot '{snapshot_name}'. Call memory_snapshot() first."

    fmt = {8: ">B", 16: ">H", 32: ">I"}
    if width not in fmt:
        return "FAIL: width must be 8, 16, or 32"

    step = width // 8
    pack_fmt = fmt[width]
    target = struct.pack(pack_fmt, value & ((1 << width) - 1))

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
    """Compare two memory snapshots to find values that changed.

    This is the 'Search Filter' step in cheat-engine workflow.

    Args:
        old_snapshot: Name of the 'before' snapshot.
        new_snapshot: Name of the 'after' snapshot. Use 'live' to take a fresh dump now.
        mode: Filter mode — 'changed', 'increased', 'decreased', 'unchanged'.
        width: Comparison width in bits — 8, 16, or 32.

    Returns addresses where values match the filter (up to 50 results).
    """
    old_data = _memory_snapshots.get(old_snapshot)
    if old_data is None:
        return f"FAIL: No snapshot '{old_snapshot}'. Call memory_snapshot() first."

    # If 'live', take a fresh snapshot now
    if new_snapshot == "live":
        result = await memory_snapshot("_live_compare")
        if result.startswith("FAIL"):
            return result
        new_data = _memory_snapshots["_live_compare"]
    else:
        new_data = _memory_snapshots.get(new_snapshot)
        if new_data is None:
            return f"FAIL: No snapshot '{new_snapshot}'. Call memory_snapshot('{new_snapshot}') first."

    if len(old_data) != len(new_data):
        return f"FAIL: Snapshot size mismatch ({len(old_data)} vs {len(new_data)})"

    step = width // 8
    if width not in (8, 16, 32):
        return "FAIL: width must be 8, 16, or 32"

    fmt = {8: ">B", 16: ">H", 32: ">I"}[width]
    sfmt = {8: ">b", 16: ">h", 32: ">i"}[width]

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

    w = width // 4  # hex digits
    lines = [f"Found {len(matches)} matches (filter: {mode}, {width}-bit):"]
    for addr, old_v, new_v in matches:
        lines.append(f"  0x{addr:08X}: 0x{old_v:0{w}X} -> 0x{new_v:0{w}X} (delta={new_v - old_v:+d})")
    if len(matches) == 50:
        lines.append("  (truncated at 50)")

    return "\n".join(lines)


@mcp.tool()
async def memory_filter_candidates(candidates: str, new_snapshot: str = "live",
                              mode: str = "changed", width: int = 32) -> str:
    """Narrow down a list of candidate addresses using a new comparison.

    For iterative cheat-engine workflow: search -> filter -> filter -> found it.

    Args:
        candidates: Comma-separated hex addresses (e.g. '0x06078900,0x0607890C,0x06078910')
        new_snapshot: Snapshot to compare against. 'live' takes a fresh dump.
        mode: Filter — 'changed', 'increased', 'decreased', 'unchanged'.
        width: Bit width — 8, 16, or 32.

    Returns only the addresses that match the filter.
    """
    s, err = _require_session()
    if err:
        return err

    # Parse candidate addresses
    addrs = []
    for addr_str in candidates.split(","):
        addr_str = addr_str.strip().replace("0x", "").replace("0X", "")
        if addr_str:
            addrs.append(int(addr_str, 16))

    if not addrs:
        return "FAIL: No candidate addresses provided"

    # Get the 'before' value at each address
    step = width // 8
    fmt = {8: ">B", 16: ">H", 32: ">I"}[width]

    before_vals = {}
    for addr in addrs:
        out_path = os.path.join(s.ipc_dir, "filter_read.bin")
        out_wsl = wsl_path(out_path)
        a = f"{addr:08X}"
        ack = await s.send_and_wait(f"dump_mem_bin {a} {step:x} {out_wsl}", "dump_mem_bin", timeout=5)
        if ack:
            await asyncio.sleep(0.1)
            if os.path.exists(out_path):
                with open(out_path, "rb") as f:
                    d = f.read(step)
                if len(d) == step:
                    before_vals[addr] = struct.unpack(fmt, d)[0]

    return (f"OK: Captured {len(before_vals)} 'before' values. "
            f"Now do something in the game, then call memory_filter_candidates_apply() "
            f"to read 'after' values and filter.\n"
            f"Addresses: {', '.join(f'0x{a:08X}' for a in before_vals.keys())}\n"
            f"Values: {', '.join(f'0x{v:08X}' for v in before_vals.values())}")


@mcp.tool()
async def raw_command(command: str) -> str:
    """Send a raw automation command string to Mednafen.
    For commands not wrapped by other tools. Returns the ack response.

    Example: raw_command('dump_mem 06078900 100')
    """
    s, err = _require_session()
    if err:
        return err
    # Try to guess a reasonable keyword to wait for
    cmd_word = command.strip().split()[0] if command.strip() else ""
    ack = await s.send_and_wait(command, cmd_word, timeout=15)
    if ack:
        return ack
    # Fallback: wait for any ack change
    await asyncio.sleep(1)
    try:
        with open(s.ack_file) as f:
            return f.read().strip()
    except (IOError, FileNotFoundError):
        return "FAIL: no response"


# ===========================================================================
# Entry point
# ===========================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Mednafen Saturn MCP Server — debug bridge for Claude Code"
    )
    parser.add_argument(
        "--transport", default="stdio", choices=["stdio", "sse"],
        help="MCP transport (default: stdio)",
    )
    parser.add_argument(
        "--cue", default=None,
        help="Default disc image path (Windows path)",
    )
    parser.add_argument(
        "--mednafen-path", default=None,
        help="Path to Mednafen binary (default: auto-detect from repo)",
    )
    parser.add_argument(
        "--ipc-dir", default=None,
        help="IPC directory (default: system temp)",
    )
    args = parser.parse_args()

    # Apply config
    if args.cue:
        _config["default_cue"] = args.cue
    if args.mednafen_path:
        _config["mednafen_path"] = args.mednafen_path
    if args.ipc_dir:
        _config["ipc_base"] = args.ipc_dir

    logging.basicConfig(level=logging.INFO, stream=sys.stderr)
    logger.info("Starting Mednafen MCP server...")
    if _config.get("default_cue"):
        logger.info(f"Default CUE: {_config['default_cue']}")

    # Monkey-patch: log every JSON-RPC message at the protocol level
    from contextlib import asynccontextmanager
    import anyio
    import mcp.server.stdio as _stdio_mod
    _orig_stdio_server = _stdio_mod.stdio_server

    @asynccontextmanager
    async def _logging_stdio_server(stdin=None, stdout=None):
        async with _orig_stdio_server(stdin, stdout) as (read_stream, write_stream):
            logged_writer, logged_reader = anyio.create_memory_object_stream(0)

            async def _log_relay():
                try:
                    async with logged_writer:
                        async for msg in read_stream:
                            if isinstance(msg, Exception):
                                MCPLOG(f"PROTO-IN: exception={msg}")
                            else:
                                MCPLOG(f"PROTO-IN: {str(msg.message)[:200]}")
                            await logged_writer.send(msg)
                except Exception as e:
                    MCPLOG(f"PROTO-IN: relay error={e}")

            async with anyio.create_task_group() as tg:
                tg.start_soon(_log_relay)
                yield logged_reader, write_stream

    _stdio_mod.stdio_server = _logging_stdio_server

    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
