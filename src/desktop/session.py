#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Pause every alias of the same Casper device; resume the canonical one."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import sys


def main():
    if os.geteuid() == 0 or sys.argv[1:] not in (["pause"], ["resume"]):
        sys.exit("Run as desktop user: python3 desktop/session.py pause|resume")
    loader = importlib.machinery.SourceFileLoader("essx_volume_session", str(Path(__file__).with_name("essx-volume")))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    volume = importlib.util.module_from_spec(spec)
    loader.exec_module(volume)
    try:
        volume.Audio().session(sys.argv[1])
    except (volume.Error, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        sys.exit("Session: " + str(exc))


if __name__ == "__main__":
    main()
