#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
python3 desktop/essx-desktop remove
# Restore the original ESSXSOF software output, leaving the working kernel intact.
python3 src/desktop/essx-volume install
python3 src/desktop/essx-volume start --wait
printf '\nESSXSOF desktop layer removed; original ESSXSOF playback restored.\n'
