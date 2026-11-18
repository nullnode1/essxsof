#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Full uninstall, including the native driver. The original native uninstaller
# remains the authority for deleting installed modules and manifests.
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" -ne 0 ] || { echo 'Run as your desktop user, not root.' >&2; exit 1; }
printf 'This removes the ESSXSOF desktop layer AND kernel driver.\n'
printf 'Type REMOVE to proceed: '
read -r answer
[ "$answer" = REMOVE ] || { echo 'Cancelled.'; exit 1; }
python3 desktop/essx-desktop remove
# Close OSS device handles before the safe native detach. If the user's
# existing controller has a truncated PID check, run the bundled fixed
# controller for a controlled stop BEFORE calling native-uninstall.sh.
pulseaudio --kill >/dev/null 2>&1 || :
if ! python3 - "$PWD/src/sbin/essxsofctl" <<'PY'
import shlex, subprocess, sys
controller = shlex.quote(sys.argv[1])
try:
    subprocess.run(['su', 'root', '-c', '/bin/sh ' + controller + ' stop'], check=True)
except subprocess.CalledProcessError:
    sys.exit(1)
PY
then
    pulseaudio --start >/dev/null 2>&1 || :
    echo 'Native stop failed; files were NOT removed. PulseAudio restart was attempted.' >&2
    exit 1
fi
exec python3 src/setup.py uninstall
