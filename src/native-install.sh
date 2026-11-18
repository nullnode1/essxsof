#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
dest=${DESTDIR:-}
share="$dest/usr/local/share/essxsof"
die() { echo "install: $*" >&2; exit 1; }
if [ -z "$dest" ]; then
    [ "$(id -u)" -eq 0 ] || die 'root required'
    [ "$(uname -s)" = FreeBSD ] && [ "$(uname -m)" = amd64 ] || die 'FreeBSD amd64 required'
    if kldstat -q -n essxsof.ko || kldstat -q -n essx8336_sof.ko; then
        die 'close players and stop the old driver with its rollback script/service before installation'
    fi
fi
[ -f out/build-info ] || die 'run sh build.sh first'
release=$(awk -F= '$1=="driver_version" {print $2}' out/build-info)
[ "$release" = 0.20.0-rc1 ] || die 'build-info version mismatch'
[ "$(awk -F= '$1=="machine" {print $2}' out/build-info)" = amd64 ] || die 'module architecture mismatch'
if [ -z "$dest" ]; then
    built=$(awk -F= '$1=="kernel_osreldate" {print $2}' out/build-info)
    [ "$built" = "$(sysctl -n kern.osreldate)" ] || die 'running kernel changed; rebuild'
fi
for pair in driver_sha256:out/essxsof.ko firmware_module_sha256:out/essx8336_sof.ko; do
    key=${pair%%:*}; file=${pair#*:}
    expected=$(awk -F= -v k="$key" '$1==k {print $2}' out/build-info)
    [ "$(sha256 -q "$file")" = "$expected" ] || die "checksum mismatch: $file"
done
# Refuse name collisions with files not owned by this package.
files='boot/modules/essxsof.ko boot/modules/essx8336_sof.ko usr/local/sbin/essxsofctl usr/local/etc/rc.d/essxsof usr/local/etc/rc.d/essxsoflate usr/local/libexec/essxsof/pcm-test usr/local/libexec/essxsof/essxsof-jackd'
if [ ! -f "$share/INSTALL-MANIFEST" ]; then
    for file in $files; do
        [ ! -e "$dest/$file" ] && [ ! -L "$dest/$file" ] || die "unmanaged existing file: /$file"
    done
else
    [ "$(head -n 1 "$share/INSTALL-MANIFEST")" = 'ESSXSOF-MANIFEST-1' ] || die 'unknown installation marker'
    backup="$dest/var/backups/essxsof/$(date -u +%Y%m%dT%H%M%SZ).$$"
    for file in $files; do
        if [ -f "$dest/$file" ]; then
            install -d -m 755 "$backup/$(dirname "$file")"
            cp -p "$dest/$file" "$backup/$file"
        fi
    done
    echo "Previous installed executables/modules backed up: $backup"
fi
put() {
    mode=$1; source=$2; path=$3
    install -d -m 755 "$(dirname "$dest/$path")"
    install -m "$mode" "$source" "$dest/$path.new.$$"
    mv -f "$dest/$path.new.$$" "$dest/$path"
}
put 644 out/essxsof.ko boot/modules/essxsof.ko
put 644 out/essx8336_sof.ko boot/modules/essx8336_sof.ko
put 755 sbin/essxsofctl usr/local/sbin/essxsofctl
put 755 rc.d/essxsof usr/local/etc/rc.d/essxsof
put 755 rc.d/essxsoflate usr/local/etc/rc.d/essxsoflate
put 755 out/pcm-test usr/local/libexec/essxsof/pcm-test
put 755 sbin/essxsof-jackd usr/local/libexec/essxsof/essxsof-jackd
put 644 etc/essxsof.conf.sample usr/local/etc/essxsof.conf.sample
if [ ! -e "$dest/usr/local/etc/essxsof.conf" ]; then
    put 644 etc/essxsof.conf.sample usr/local/etc/essxsof.conf
fi
put 644 ../README.md usr/local/share/essxsof/README.md
put 644 ../LICENSE usr/local/share/essxsof/LICENSE
for name in GPL-2.0.txt BSD-2-Clause.txt SOF.txt Intel-firmware.txt; do
    put 644 "../licenses/$name" "usr/local/share/essxsof/$name"
done
put 644 firmware/LICENCE.Intel usr/local/share/essxsof/LICENCE.Intel
put 644 vendor/LICENCE.SOF usr/local/share/essxsof/LICENCE.SOF
put 644 out/build-info usr/local/share/essxsof/build-info
{
    echo 'ESSXSOF-MANIFEST-1'
    for file in $files; do printf '%s %s\n' "$(sha256 -q "$dest/$file")" "$file"; done
} > "$share/INSTALL-MANIFEST"
echo 'Native files installed.'
