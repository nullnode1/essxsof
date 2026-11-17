#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
[ "$(uname -s)" = FreeBSD ] && [ "$(uname -m)" = amd64 ] || { echo 'FreeBSD amd64 required.' >&2; exit 1; }
rm -f out/build-info
sysdir=${SYSDIR:-/usr/src/sys}
running=$(sysctl -n kern.osreldate)
source_version=$(awk '$1=="#define" && $2=="__FreeBSD_version" {print $3; exit}' "$sysdir/sys/param.h")
[ "$source_version" = "$running" ] || { echo "Kernel/source version mismatch ($running / $source_version). Install matching /usr/src first." >&2; exit 1; }
[ "$(sha256 -q firmware/sof-tgl.ri)" = 97c8d8dea6048329bfbb1dbbd56609934d388b7e3a9acffe20df0b7daf14d7d3 ] || { echo 'Firmware checksum mismatch.' >&2; exit 1; }
make clean SYSDIR="$sysdir"
make -C firmware-module clean SYSDIR="$sysdir"
make SYSDIR="$sysdir"
make -C firmware-module SYSDIR="$sysdir"
cc -O2 -Wall -Wextra -o pcm-test pcm-test.c
mkdir -p out
cp essxsof.ko out/essxsof.ko
cp firmware-module/essx8336_sof.ko out/essx8336_sof.ko
cp pcm-test out/pcm-test
{
    echo 'driver_version=0.19.0-rc1'
    echo "kernel_osreldate=$running"
    echo "source_osreldate=$source_version"
    echo 'machine=amd64'
    echo "driver_sha256=$(sha256 -q out/essxsof.ko)"
    echo "firmware_module_sha256=$(sha256 -q out/essx8336_sof.ko)"
} > out/build-info.new.$$
mv -f out/build-info.new.$$ out/build-info
echo 'Built: out/essxsof.ko, out/essx8336_sof.ko, out/pcm-test'
echo 'Native module build completed.'
