#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
exec python3 desktop/migrate.py
