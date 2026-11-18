#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Non-disruptive root script update for already running 0.20.0-rc1.
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" -ne 0 ] || { echo 'Run as normal desktop user; root is requested.' >&2; exit 1; }
python3 - "$PWD/src/native-repair.sh" <<'PY'
import shlex, subprocess, sys
cmd = shlex.join(['/bin/sh', sys.argv[1]])
subprocess.run(['su', 'root', '-c', cmd], check=True)
PY
