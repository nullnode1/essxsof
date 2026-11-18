#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Update ONLY user-space boot/jack scripts; preserve running native modules.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'root required' >&2; exit 1; }
[ "$(uname -s)" = FreeBSD ] || exit 1
cd "$(dirname "$0")"
manifest=/usr/local/share/essxsof/INSTALL-MANIFEST
[ -f "$manifest" ] && [ ! -L "$manifest" ] || { echo 'Cannot find managed ESSXSOF installation' >&2; exit 1; }
[ "$(head -n 1 "$manifest")" = ESSXSOF-MANIFEST-1 ] || { echo 'Unknown native manifest' >&2; exit 1; }
[ "$(sysctl -n dev.essxsof.0.driver_version 2>/dev/null || :)" = 0.20.0-rc1 ] || {
    echo 'Expected 0.20.0-rc1 kernel driver running; no changes made' >&2; exit 1;
}
# No modification of third-party or local files: installed controller should be
# this version, and only controlled package paths are admitted.
ctl=/usr/local/sbin/essxsofctl
jack=/usr/local/libexec/essxsof/essxsof-jackd
rc=/usr/local/etc/rc.d/essxsof
late=/usr/local/etc/rc.d/essxsoflate
for f in "$ctl" "$jack" "$rc"; do
    [ -f "$f" ] && [ ! -L "$f" ] || { echo "Missing/unexpected managed file: $f" >&2; exit 1; }
done
grep -q '^version=0.20.0-rc1$' "$ctl" || { echo 'Controller version mismatch' >&2; exit 1; }
grep -q "VERSION = '0.20.0-rc1'" "$jack" || { echo 'Monitor version mismatch' >&2; exit 1; }
[ ! -e "$late" ] || cmp -s "$late" rc.d/essxsoflate || {
    echo 'A different late-start script already exists; refusing overwrite' >&2; exit 1;
}
# Check every manifest path before making modifications.
while read -r sum file; do
    [ "$sum" != ESSXSOF-MANIFEST-1 ] || continue
    case "$file" in
        boot/modules/essxsof.ko|boot/modules/essx8336_sof.ko|usr/local/sbin/essxsofctl|usr/local/etc/rc.d/essxsof|usr/local/etc/rc.d/essxsoflate|usr/local/libexec/essxsof/pcm-test|usr/local/libexec/essxsof/essxsof-jackd) ;;
        *) echo "Unknown managed path: $file" >&2; exit 1;;
    esac
done < "$manifest"
# Update package manifest checksum in sync with the new root-owned scripts.
backup="/var/backups/essxsof/bootfix-$(date -u +%Y%m%dT%H%M%SZ)-$$"
install -d -m 700 "$backup"
cp -p "$manifest" "$backup/INSTALL-MANIFEST"
for f in "$ctl" "$jack" "$rc"; do cp -p "$f" "$backup/$(basename "$f")"; done
if [ -f "$late" ]; then cp -p "$late" "$backup/essxsoflate"; fi
replace() {
    src=$1; dest=$2
    install -o root -g wheel -m 755 "$src" "$dest.new.$$"
    mv -f "$dest.new.$$" "$dest"
}
replace sbin/essxsofctl "$ctl"
replace sbin/essxsof-jackd "$jack"
replace rc.d/essxsof "$rc"
replace rc.d/essxsoflate "$late"
# Atomic, exact manifest update; no kernel blobs changed.
tmp="$manifest.new.$$"
awk '
    NR==1 {print; next}
    $2!="usr/local/sbin/essxsofctl" &&
    $2!="usr/local/libexec/essxsof/essxsof-jackd" &&
    $2!="usr/local/etc/rc.d/essxsof" &&
    $2!="usr/local/etc/rc.d/essxsoflate" {print}
' "$manifest" > "$tmp"
for file in usr/local/sbin/essxsofctl usr/local/libexec/essxsof/essxsof-jackd usr/local/etc/rc.d/essxsof usr/local/etc/rc.d/essxsoflate; do
    printf '%s %s\n' "$(sha256 -q "/$file")" "$file" >> "$tmp"
done
chmod 644 "$tmp"
mv -f "$tmp" "$manifest"
/bin/sh -n "$ctl" "$rc" "$late" 2>/dev/null || /bin/sh -n "$ctl"
printf 'Updated ESSXSOF root service scripts. No native module was reloaded.\n'
printf 'Backup: %s\n' "$backup"
printf 'Late boot retry enabled (essxsoflate, requires essxsof_enable=YES).\n'
/usr/local/sbin/essxsofctl status
