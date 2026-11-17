#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
command -v python3 >/dev/null 2>&1 || { echo 'Python 3 is required; see README.md.' >&2; exit 1; }
exec python3 src/setup.py uninstall
