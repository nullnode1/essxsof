#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" -eq 0 ] || { echo 'Root is required for native changes.' >&2; exit 1; }
[ "$(uname -s)" = FreeBSD ] && [ "$(uname -m)" = amd64 ] || exit 1
case "${1:-}" in
    install)
        if [ -x /usr/local/sbin/essxsofctl ]; then
            /usr/local/sbin/essxsofctl stop
        fi
        sh native-install.sh
        service essxsof onestart
        /usr/local/sbin/essxsofctl status
        sysrc essxsof_enable=YES
        ;;
    uninstall)
        if [ -f /usr/local/share/essxsof/INSTALL-MANIFEST ]; then
            sh native-uninstall.sh
        elif kldstat -q -n essxsof.ko || kldstat -q -n essx8336_sof.ko; then
            echo 'A non-package audio module is loaded; stopped without forcing.' >&2
            exit 1
        else
            sysrc essxsof_enable=NO
        fi
        ;;
    *) echo 'Expected install or uninstall.' >&2; exit 1;;
esac
