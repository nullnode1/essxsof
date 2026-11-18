# ESSXSOF 1.0.0 — validation and acceptance

## Field evidence (original Casper Nirvana NB F500, FreeBSD 15.1)

- Native `0.20.0-rc1` kernel C playback driver repeatedly produced working speaker and wired-headphone sound.
- Plug/unplug transitions were heard by the owner and observed in the kernel and jack-monitor logs.
- RC2 booted successfully with the headphones connected: the kernel initialized, followed by the monitor and two logical desktop outputs; PCM error counter remained zero.
- RC5 root and ordinary-user status reported active jack monitoring; the desktop outputs were present and the physical routing selected the headset.
- RC5 installer exited nonzero during optional PulseAudio duplicate cleanup due to a module-identity race; the subsequent diagnostic report showed three expected output sinks and PCM fault=0.

## What has not been verified

- The final 1.0.0 cleanup fix has only undergone offline simulation, not on-hardware installation.
- General ES8336 notebook compatibility, analog microphone capture, long-term suspend/resume, and full uninstall on the daily machine are unverified/not supported.
- Native FreeBSD build is performed on the target machine; these Linux-container tests cannot validate FreeBSD kernel ABI correctness.

## Offline tests

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 -m unittest discover -s src/tests -p '*offline.py' -v
sh -n install.sh uninstall.sh uninstall-desktop.sh repair-boot.sh desktop-upgrade.sh diagnose.sh
```

## Final installed-system checks (FreeBSD 15.1)

```sh
sh install.sh
sh diagnose.sh
essxsofctl status
~/.local/bin/essx-desktop status
pactl list short sinks
pactl list short sources
sysctl dev.pcm.0.essx_fault
```

Acceptance criteria:

1. Cold boot with analog headphones unplugged: speakers play.
2. Plug in while playing: speakers mute and headphones play, without simultaneous output.
3. Repeat ten transitions, then boot with headphones plugged in; automatic routing still works.
4. Three outputs remain (two logical profiles, one 48 kHz backend) or any additional ESSXSOF-only duplicates are reported without deleting unrelated devices.
5. `dev.pcm.0.essx_fault` remains 0; any USB capture device stays visible.
6. 20–30 minutes of uninterrupted playback at a comfortable volume.
7. Installation completes even if PulseAudio auto-removes a duplicate OSS module during optional cleanup.
8. Full uninstall should be verified on a separate test installation rather than risking the working production driver.

If final hardware checks fail, return to the working RC5 and treat 1.0.0 as unvalidated for that system.
