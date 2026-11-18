#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# For an existing working 0.20.0-rc1 kernel driver, use desktop-upgrade.sh.
set -eu
cd "$(dirname "$0")"
if [ "$(uname -s)" != FreeBSD ]; then echo 'FreeBSD required.' >&2; exit 1; fi
if [ "$(id -u)" -eq 0 ]; then echo 'Run as a regular user, not root.' >&2; exit 1; fi
if sysctl -n dev.essxsof.0.driver_version 2>/dev/null | grep -qx '0.20.0-rc1'; then
    echo 'Working ESSXSOF 0.20.0-rc1 kernel detected. Keeping it intact.'
    /bin/sh repair-boot.sh
else
    echo 'Installing the ESSXSOF kernel driver and base desktop layer.'
    python3 src/setup.py install
fi
exec /bin/sh desktop-upgrade.sh
