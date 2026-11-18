#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Read-only after-boot health report; safe as an ordinary user.
set -u
printf '\n=== Operating system ===\n'
uname -srmp
printf '\n=== ESSXSOF driver / jack daemon ===\n'
essxsofctl status 2>&1 || :
printf '\n=== Boot configuration ===\n'
sysrc essxsof_enable 2>&1 || :
grep '^essxsof_jack_auto=' /usr/local/etc/essxsof.conf 2>&1 || :
printf '\n=== Running services ===\n'
ps axww | grep -E '[e]ssxsof-jackd|[e]ssx-desktop run' || :
printf '\n=== Desktop audio ===\n'
"$HOME/.local/bin/essx-desktop" status 2>&1 || :
printf '\n=== PulseAudio outputs ===\n'
pactl list short sinks 2>&1 || :
printf '\n=== PulseAudio inputs ===\n'
pactl list short sources 2>&1 || :
printf '\n=== Kernel PCM errors ===\n'
sysctl dev.pcm.0.essx_fault 2>&1 || :
printf '\n=== Recent ESSXSOF system messages ===\n'
grep -Ei 'essxsof|essxcodec|essxsoflate' /var/log/messages 2>/dev/null | tail -n 40 || :
