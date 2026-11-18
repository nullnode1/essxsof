#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Conservative desktop migration from ESSXSOF preview tools to the playback release."""
from pathlib import Path
from datetime import datetime, timezone
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NEW = ROOT / "desktop/essx-desktop"
LEGACY_VOLUME = ROOT / "src/desktop/essx-volume"
HOME = Path.home()
BINDIR = HOME / ".local/bin"
AUTOSTART = HOME / ".config/autostart"
BACKUP_ROOT = HOME / ".local/state/essxsof-release/backups"
FILES = [BINDIR / "essx-ports", AUTOSTART / "essx-ports.desktop",
         BINDIR / "essx-volume", AUTOSTART / "essx-volume.desktop"]


def run(*cmd):
    print("$", " ".join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True)


def backup():
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    target = BACKUP_ROOT / (stamp + "-" + str(os.getpid()))
    target.mkdir(parents=True, exist_ok=False, mode=0o700)
    for i, path in enumerate(FILES):
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, target / f"file-{i}")
    print("Previous desktop scripts backed up to:", target)
    return target


def restore(target):
    for i, path in enumerate(FILES):
        src = target / f"file-{i}"
        if src.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, path)
        elif path.exists() and not path.is_symlink():
            # Only remove known old autostart files when restoring after failure.
            if path.name in ("essx-ports.desktop", "essx-volume.desktop"):
                content = path.read_text(encoding="utf-8", errors="replace")
                if "X-Essx-Ports=true" in content or "X-Essx-Volume=true" in content:
                    path.unlink()


def optional_cleanup():
    """Duplicate sink removal is cosmetic. Installation succeeds if outputs work.

    Device discovery might change the module list while we clean it. All
    destructive cleanup is guarded by identity checks inside essx-desktop.
    """
    cmd = [sys.executable, str(BINDIR / "essx-desktop"), "cleanup", "--apply"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.returncode:
        print("WARNING: Optional duplicate cleanup did not finish. Audio profiles remain active.", file=sys.stderr)
        if result.stderr:
            print(result.stderr.strip(), file=sys.stderr)
        print("Use 'essx-desktop doctor' to inspect remaining outputs.", file=sys.stderr)
    return result.returncode == 0


def migrate():
    if os.geteuid() == 0 or not sys.platform.startswith("freebsd"):
        raise RuntimeError("Run as the normal desktop user on FreeBSD")
    run("essxsofctl", "status")
    run("pactl", "info")
    installed = BINDIR / "essx-desktop"
    if installed.is_file():
        if "ESSXSOF user-session audio profiles" not in installed.read_text(encoding="utf-8"):
            raise RuntimeError("An unrelated essx-desktop program already exists")
        current = subprocess.run(["pactl", "list", "short", "sinks"],
                                 capture_output=True, text=True, check=True).stdout
        # Replace only a recognized prior ESSXSOF desktop executable.
        # Stop monitor before replacing its on-disk executable.
        print("Updating existing ESSXSOF desktop component without touching kernel.")
        run(sys.executable, installed, "stop")
        run(sys.executable, NEW, "install")
        run(sys.executable, BINDIR / "essx-desktop", "start")
        optional_cleanup()
        run(sys.executable, BINDIR / "essx-desktop", "status")
        return
    backed = backup()
    old_ports = BINDIR / "essx-ports"
    old_volume = BINDIR / "essx-volume"
    had_ports = old_ports.exists()
    had_volume = old_volume.exists()
    try:
        # Old preview must be stopped FIRST; it depends on the old volume helper.
        if had_ports:
            if "ESSXSOF logical speaker/headphone ports" not in old_ports.read_text(encoding="utf-8"):
                raise RuntimeError("Unrecognized old ports script; no files were modified")
            run(sys.executable, old_ports, "stop")
            # We do not automatically erase the old script; backup is retained.
            if (AUTOSTART / "essx-ports.desktop").exists():
                content = (AUTOSTART / "essx-ports.desktop").read_text(encoding="utf-8")
                if "X-Essx-Ports=true" not in content:
                    raise RuntimeError("Unknown ports autostart; refusing to edit")
                (AUTOSTART / "essx-ports.desktop").unlink()
        if had_volume:
            if "Optional per-user PulseAudio software output" not in old_volume.read_text(encoding="utf-8"):
                raise RuntimeError("Unrecognized old ESSXSOF volume helper; no audio module unloaded")
            run(sys.executable, old_volume, "remove")
        elif "essx_volume_200" in subprocess.run(["pactl", "list", "short", "sinks"],
                                                capture_output=True, text=True, check=True).stdout:
            raise RuntimeError("Unmanaged legacy software output is active; cannot migrate safely")
        run(sys.executable, NEW, "install")
        run(sys.executable, BINDIR / "essx-desktop", "start")
        # The user's exact duplicate /dev/dsp0 modules may now be removed.
        # Does NOT touch dsp1 recording, USB or unrelated audio modules.
        optional_cleanup()
        run(sys.executable, BINDIR / "essx-desktop", "status")
    except Exception:
        print("Migration failed. Attempting to restore the previous desktop configuration.", file=sys.stderr)
        try:
            subprocess.run([sys.executable, str(NEW), "remove"], check=False)
            restore(backed)
            if had_volume and old_volume.exists():
                run(sys.executable, old_volume, "start", "--wait")
            if had_ports and old_ports.exists():
                run(sys.executable, old_ports, "start")
        except Exception as recovery:
            print("Automatic desktop recovery incomplete:", recovery, file=sys.stderr)
            print("Saved backup:", backed, file=sys.stderr)
        raise
    print("\nMigration complete. You may leave this backup in place:", backed)
    print("Expected: Speakers, Headphones, plus one technical 48 kHz backend.")
    print("If other sinks remain, inspect: essx-desktop doctor")

if __name__ == "__main__":
    try:
        migrate()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit("ESSXSOF upgrade stopped: " + str(exc))
