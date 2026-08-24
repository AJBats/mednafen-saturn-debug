#!/usr/bin/env python3
"""Shared Mednafen automation bot.

Drives Windows Mednafen via file-based IPC (action/ack files).
All tools that need to launch and control Mednafen should import from here.
"""

import os
import sys
import time
import subprocess
import tempfile

MEDNAFEN_DIR = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_HOME = os.path.join(MEDNAFEN_DIR, "home")


def _is_wsl():
    """True when running on Linux under WSL (Windows-backed)."""
    if not sys.platform.startswith("linux"):
        return False
    try:
        with open("/proc/version") as f:
            return "microsoft" in f.read().lower()
    except OSError:
        return False


def _win_native_home(seed_home):
    """Windows-native emulator home for running the Windows exe under WSL.

    The exe rejects \\\\wsl.localhost UNC homes, so mirror the requested
    home's cfg + firmware to %LOCALAPPDATA%\\mednafen_autore\\<slug>.
    sound.driver is reset to default so Windows picks its native audio
    path (the whole point of using the exe: WSLg's audio bridge is broken).
    Saves (sav/) persist across runs there — desirable for interactive play.
    Returns the WSL-visible path, or None if resolution fails."""
    try:
        out = subprocess.check_output(
            ["cmd.exe", "/c", "echo %LOCALAPPDATA%"],
            cwd="/mnt/c/", stderr=subprocess.DEVNULL, text=True).strip()
        if not out or out.startswith("%"):
            return None
        base = subprocess.check_output(["wslpath", out], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    seed = os.path.abspath(seed_home)
    slug = seed.strip("/").replace("/", "_")[-80:]
    home = os.path.join(base, "mednafen_autore", slug)
    os.makedirs(os.path.join(home, "firmware"), exist_ok=True)
    src = seed if os.path.isfile(os.path.join(seed, "mednafen.cfg")) else _DEFAULT_HOME
    with open(os.path.join(src, "mednafen.cfg")) as f:
        cfg = f.read()
    cfg = cfg.replace("sound.driver sdl", "sound.driver default")
    with open(os.path.join(home, "mednafen.cfg"), "w", newline="\n") as f:
        f.write(cfg)
    fw_src = os.path.join(src, "firmware")
    if os.path.isdir(fw_src):
        for fn in os.listdir(fw_src):
            dst = os.path.join(home, "firmware", fn)
            if not os.path.exists(dst):
                import shutil
                shutil.copy2(os.path.join(fw_src, fn), dst)
    return home


def _find_mednafen(interactive_sound=False):
    """Find the Mednafen executable.

    On true Linux the canonical binary is the native ELF build
    (src/mednafen). Under WSL it is too — except for interactive-with-sound
    sessions, which use the Windows exe: WSLg's RDP audio bridge wedges
    under sustained streams (microsoft/wslg#1429), while the exe plays
    through the native Windows audio stack. The GCC 4.9.4 Windows exes also
    remain the reference artifacts matching the stock 1.32.1 build env."""
    if sys.platform.startswith("linux") and not (interactive_sound and _is_wsl()):
        native = os.path.join(MEDNAFEN_DIR, "src", "mednafen")
        if os.path.exists(native):
            return native
    # Debug build (unstripped, with symbols for crash dumps)
    debug = os.path.join(MEDNAFEN_DIR, "mednafen_debug.exe")
    if os.path.exists(debug):
        return debug
    # Release build in src/ (freshly compiled)
    primary = os.path.join(MEDNAFEN_DIR, "src", "mednafen.exe")
    if os.path.exists(primary):
        return primary
    # Release build (stable copy)
    fallback = os.path.join(MEDNAFEN_DIR, "mednafen_gcc494.exe")
    if os.path.exists(fallback):
        return fallback
    return primary  # let it fail at launch time with a clear path


def _win_path(p):
    """Normalize path for Windows Mednafen (forward slashes)."""
    return p.replace("\\", "/")


class MednafenBot:
    """Drives Windows Mednafen via file-based automation IPC."""

    def __init__(self, ipc_dir, cue_path, show=False, sound=False,
                 home_dir=None, verbose=False):
        self.ipc_dir = ipc_dir
        self.action_file = os.path.join(ipc_dir, "mednafen_action.txt")
        self.ack_file = os.path.join(ipc_dir, "mednafen_ack.txt")
        self.seq = 0
        self.last_ack = ""
        self.proc = None
        self.stderr_file = None
        self.cue_path = cue_path
        self.show = show
        self.sound = sound
        self.home_dir = home_dir or _DEFAULT_HOME
        self.verbose = verbose

    def start(self, timeout=45):
        """Launch Mednafen and wait for ready ack."""
        med_bin = _find_mednafen(interactive_sound=self.sound)
        os.makedirs(self.ipc_dir, exist_ok=True)
        os.makedirs(self.home_dir, exist_ok=True)

        # Windows exe under WSL cannot use a \\wsl.localhost UNC home —
        # redirect to a Windows-native mirror of the requested home.
        home_dir = self.home_dir
        if med_bin.endswith(".exe") and sys.platform.startswith("linux"):
            wh = _win_native_home(self.home_dir)
            if wh:
                home_dir = wh

        # Remove stale lockfile
        lockfile = os.path.join(home_dir, "mednafen.lck")
        try:
            if os.path.exists(lockfile):
                os.remove(lockfile)
        except PermissionError:
            pass

        # Clean IPC files — remove ALL files in IPC dir to prevent stale
        # artifacts from previous sessions (traces, snapshots, etc.)
        for f in os.listdir(self.ipc_dir):
            fpath = os.path.join(self.ipc_dir, f)
            try:
                if os.path.isfile(fpath):
                    os.remove(fpath)
            except (PermissionError, OSError):
                pass

        env = os.environ.copy()
        env["MEDNAFEN_HOME"] = home_dir
        # Point crash dumps to the SaturnAutoRE crash_dumps dir
        crash_dir = os.path.join(MEDNAFEN_DIR, "..", "crash_dumps")
        os.makedirs(crash_dir, exist_ok=True)
        env["MEDNAFEN_CRASH_DUMP_DIR"] = os.path.abspath(crash_dir)
        # When running the Windows exe from inside WSL, env vars only cross
        # the interop boundary if listed in WSLENV ("/p" = translate path).
        # Without this Mednafen silently falls back to the shared Windows
        # %USERPROFILE%\.mednafen home (stateful saves -> nondeterminism).
        if sys.platform.startswith("linux"):
            wslenv = env.get("WSLENV", "")
            for var in ("MEDNAFEN_HOME/p", "MEDNAFEN_CRASH_DUMP_DIR/p"):
                if var not in wslenv:
                    wslenv = f"{wslenv}:{var}" if wslenv else var
            env["WSLENV"] = wslenv

        self.stderr_file = tempfile.NamedTemporaryFile(
            mode="w", suffix="_mednafen_stderr.txt", delete=False,
        )
        self.proc = subprocess.Popen(
            [med_bin, "--sound", "1" if self.sound else "0",
             "-cd.image_memcache", "1",
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
