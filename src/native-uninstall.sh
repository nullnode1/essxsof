#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
dest=${DESTDIR:-}
share="$dest/usr/local/share/essxsof"
[ -f "$share/INSTALL-MANIFEST" ] || { echo 'No essxsof package installation found.' >&2; exit 1; }
[ "$(head -n 1 "$share/INSTALL-MANIFEST")" = ESSXSOF-MANIFEST-1 ] || exit 1
if [ -z "$dest" ]; then
    [ "$(id -u)" -eq 0 ] || { echo 'root required' >&2; exit 1; }
    # If the driver refuses a safe detach, leave all installed files intact.
    /usr/local/sbin/essxsofctl stop
    if [ "$(sysrc -n essxsof_enable 2>/dev/null || :)" = YES ]; then sysrc essxsof_enable=NO; fi
fi
preserved=0
while read -r checksum file; do
    [ "$checksum" != ESSXSOF-MANIFEST-1 ] || continue
    case "$file" in
        boot/modules/essxsof.ko|boot/modules/essx8336_sof.ko|usr/local/sbin/essxsofctl|usr/local/etc/rc.d/essxsof|usr/local/libexec/essxsof/pcm-test) ;;
        *) echo 'Unexpected manifest path; stopped.' >&2; exit 1;;
    esac
    if [ -f "$dest/$file" ]; then
        if [ "$(sha256 -q "$dest/$file")" = "$checksum" ]; then rm -f "$dest/$file";
        else echo "Preserved modified file: /$file"; preserved=1; fi
    fi
done < "$share/INSTALL-MANIFEST"
if [ "$preserved" -eq 0 ]; then rm -f "$share/INSTALL-MANIFEST"; fi
echo 'Uninstalled matching modules/control files. Configuration, documentation and backups were preserved.'
