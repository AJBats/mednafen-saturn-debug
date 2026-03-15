#!/usr/bin/env python3
"""Shared Mednafen automation bot.

Drives Windows Mednafen via file-based IPC (action/ack files).
All tools that need to launch and control Mednafen should import from here.
"""

import os
import time
import subprocess
import tempfile

MEDNAFEN_DIR = os.path.dirname(os.path.abspath(__file__))
MEDNAFEN_HOME = os.path.join(MEDNAFEN_DIR, "home")


def _find_mednafen():
    """Find the Mednafen executable."""
    primary = os.path.join(MEDNAFEN_DIR, "src", "mednafen.exe")
    if os.path.exists(primary):
        return primary
    fallback = os.path.join(MEDNAFEN_DIR, "mednafen_gcc494.exe")
    if os.path.exists(fallback):
        return fallback
    return primary  # let it fail at launch time with a clear path


def _win_path(p):
    """Normalize path for Windows Mednafen (forward slashes)."""
    return p.replace("\\", "/")


class MednafenBot:
    """Drives Windows Mednafen via file-based automation IPC."""

    def __init__(self, ipc_dir, cue_path, show=False, verbose=False):
        self.ipc_dir = ipc_dir
        self.action_file = os.path.join(ipc_dir, "mednafen_action.txt")
        self.ack_file = os.path.join(ipc_dir, "mednafen_ack.txt")
        self.seq = 0
        self.last_ack = ""
        self.proc = None
        self.stderr_file = None
        self.cue_path = cue_path
        self.show = show
        self.verbose = verbose

    def start(self, timeout=45):
        """Launch Mednafen and wait for ready ack."""
        med_bin = _find_mednafen()
        os.makedirs(self.ipc_dir, exist_ok=True)
        os.makedirs(MEDNAFEN_HOME, exist_ok=True)

        # Remove stale lockfile
        lockfile = os.path.join(MEDNAFEN_HOME, "mednafen.lck")
        try:
            if os.path.exists(lockfile):
                os.remove(lockfile)
        except PermissionError:
            pass

        # Clean IPC files
        for f in [self.action_file, self.ack_file]:
            if os.path.exists(f):
                os.remove(f)

        env = os.environ.copy()
        env["MEDNAFEN_HOME"] = MEDNAFEN_HOME

        self.stderr_file = tempfile.NamedTemporaryFile(
            mode="w", suffix="_mednafen_stderr.txt", delete=False,
        )
        self.proc = subprocess.Popen(
            [med_bin, "--sound", "0",
             "--automation", self.ipc_dir, self.cue_path],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=self.stderr_file,
            env=env,
        )

        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                print(f"  Mednafen exited with code {self.proc.returncode}")
                return False
            if os.path.exists(self.ack_file):
                try:
                    with open(self.ack_file) as f:
                        content = f.read().strip()
                    if "ready" in content:
                        self.last_ack = content
                        return True
                except (IOError, PermissionError):
                    pass
            time.sleep(0.2)

        self.proc.kill()
        return False

    def send(self, cmd):
        """Send a command via action file (with retry for Windows file locks)."""
        self.seq += 1
        padding = "." * (self.seq % 16)
        tmp = self.action_file + ".tmp"
        with open(tmp, "w", newline="\n") as f:
            f.write(f"# {self.seq}{padding}\n")
            f.write(cmd + "\n")
        for attempt in range(20):
            try:
                if os.path.exists(self.action_file):
                    os.remove(self.action_file)
                os.rename(tmp, self.action_file)
                return
            except PermissionError:
                time.sleep(0.02 * (attempt + 1))
        raise PermissionError(f"Cannot write action file after 20 retries")

    def wait_ack(self, keyword, timeout=30):
        """Wait for ack to change and contain keyword.

        keyword can be a string or list of strings (matches any).
        """
        keywords = [keyword] if isinstance(keyword, str) else keyword
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc and self.proc.poll() is not None:
                print(f"  [!] Mednafen process exited (rc={self.proc.returncode})")
                return None
            if os.path.exists(self.ack_file):
                try:
                    with open(self.ack_file) as f:
                        content = f.read().strip()
                except (IOError, PermissionError):
                    time.sleep(0.05)
                    continue
                if content != self.last_ack and any(k in content for k in keywords):
                    self.last_ack = content
                    if self.verbose:
                        print(f"  [ack] {content[:120]}")
                    return content
            time.sleep(0.05)
        print(f"  [timeout] keyword='{keyword}' last_ack='{self.last_ack[:60]}'")
        return None

    def send_and_wait(self, cmd, keyword, timeout=30):
        """Send command and wait for ack containing keyword."""
        if self.verbose:
            print(f"  [send] {cmd} (wait for '{keyword}')")
        self.send(cmd)
        return self.wait_ack(keyword, timeout)

    def frame_advance(self, n, timeout=120):
        """Advance N frames and wait for completion."""
        return self.send_and_wait(
            f"frame_advance {n}", "done frame_advance", timeout=timeout
        )

    def check_stderr(self, patterns=None):
        """Check captured stderr for fatal patterns. Returns list of matches."""
        if patterns is None:
            patterns = ["SH2-ADDRERR"]
        if not self.stderr_file:
            return []
        self.stderr_file.flush()
        errors = []
        try:
            with open(self.stderr_file.name, "r") as f:
                for line in f:
                    for pattern in patterns:
                        if pattern in line:
                            errors.append(line.strip())
                            break
        except (IOError, PermissionError):
            pass
        return errors

    def quit(self):
        """Clean shutdown."""
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
