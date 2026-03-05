#!/usr/bin/env python3
"""Mednafen MCP Server v2 — built up from hello world."""

import os
import sys
import time
import asyncio
import subprocess
import tempfile

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("mednafen")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def wsl_path(win_path):
    """Convert Windows path to WSL path."""
    if not win_path or ":" not in win_path[:3]:
        return win_path
    drive = win_path[0].lower()
    rest = win_path[2:].replace("\\", "/")
    return f"/mnt/{drive}{rest}"


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


def _send(cmd):
    """Write a command to the action file."""
    global _seq
    _seq += 1
    padding = "." * (_seq % 16)
    tmp = _action_file + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(f"# {_seq}{padding}\n")
        f.write(cmd + "\n")
    if os.path.exists(_action_file):
        os.remove(_action_file)
    os.rename(tmp, _action_file)


async def _wait_ack(keyword, timeout=30):
    """Poll ack file for a line containing keyword."""
    global _last_ack
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
            if content != _last_ack and keyword in content:
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


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def boot(cue_path: str = "", timeout: int = 45) -> str:
    """Launch Mednafen with a disc image. Starts paused at frame 0."""
    global _proc, _ack_file, _action_file, _last_ack, _seq, _frame, _ipc_dir

    if _alive():
        _send("quit")
        try:
            _proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _proc.kill()

    cue = cue_path or _default_cue or ""
    if not cue:
        return "FAIL: No disc image. Pass cue_path or use --cue."

    ipc = _ipc_dir or os.path.join(tempfile.gettempdir(), "mednafen_mcp_ipc")
    os.makedirs(ipc, exist_ok=True)
    _ack_file = os.path.join(ipc, "mednafen_ack.txt")
    _action_file = os.path.join(ipc, "mednafen_action.txt")

    for f in [_ack_file, _action_file]:
        if os.path.exists(f):
            os.remove(f)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    med_bin = os.path.join(script_dir, "src", "mednafen")

    launch = (
        f'export DISPLAY=:0; '
        f'rm -f "$HOME/.mednafen/mednafen.lck"; '
        f'"{wsl_path(med_bin)}" '
        f'--sound 0 --automation "{wsl_path(ipc)}" "{wsl_path(cue)}"'
    )

    stderr_f = tempfile.NamedTemporaryFile(mode="w", suffix="_med.txt", delete=False)
    _proc = subprocess.Popen(
        ["wsl", "-d", "Ubuntu", "-e", "bash", "-c", launch],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=stderr_f,
    )

    _last_ack = ""
    _seq = 0
    _frame = 0

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
    """Get emulator status."""
    if not _alive():
        return "No session running."
    ack = await _send_and_wait("status", "status", timeout=5)
    return f"Frame: {_frame}\n{ack or ''}"


@mcp.tool()
async def frame_advance(count: int = 1) -> str:
    """Advance N frames (default 1)."""
    global _frame
    if not _alive():
        return "FAIL: No session"
    ack = await _send_and_wait(f"frame_advance {count}", "done frame_advance", timeout=180)
    if ack:
        _frame += count
        return f"OK: Advanced {count} frames. Now at frame {_frame}"
    return "FAIL: frame_advance timed out"


@mcp.tool()
async def screenshot(output_path: str = "") -> str:
    """Screenshot the current frame (no PC movement). Returns file path."""
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
    size = os.path.getsize(shot) if os.path.exists(shot) else 0
    return f"OK: Screenshot saved to {shot} ({size} bytes)"


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
    return "OK"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--cue", default=None)
    parser.add_argument("--ipc-dir", default=None)
    args = parser.parse_args()

    global _default_cue, _ipc_dir
    if args.cue:
        _default_cue = args.cue
    if args.ipc_dir:
        _ipc_dir = args.ipc_dir

    mcp.run()


if __name__ == "__main__":
    main()
