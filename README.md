# ESSXSOF — FreeBSD Intel SOF / ES8336 Playback Driver

**Distribution: 1.0.0 — playback-only release (2026-11-18)**
**Kernel module version: 0.20.0-rc1 (unchanged from hardware-tested build)**
**Target: FreeBSD/amd64 on the originally validated Casper Nirvana NB F500 Tiger Lake / ES8336 laptop.**

This distribution bundles the original experimentally developed native driver,
its boot/service fixes and a PulseAudio desktop profile manager. Playback and
jack routing have been verified on the named laptop with the RC2–RC5 builds.
**The final 1.0.0 packaging-only cleanup change has not been independently
retested on the physical laptop.** Follow the post-install checks before
sharing installation results or claiming broader hardware support.

## Technical documentation (all English)

This repository includes source-audited explanations of **how the driver
actually works**, including codec register masks, Intel SOF firmware startup,
GPIO speaker muting, headphone detection, FreeBSD PCM, and the boot/desktop
services:

| Document | Contents |
| --- | --- |
| [`docs/HARDWARE_REGISTERS.md`](docs/HARDWARE_REGISTERS.md) | ES8336 I2C register table, exact masked writes, DAC attenuation, jack-status 0x4F, GPIO173/174, board guards, rollback. |
| [`docs/DRIVER_INTERNALS.md`](docs/DRIVER_INTERNALS.md) | Firmware boot, PCI/HDA/DSP register offsets, SOF IPC3 topology, playback DMA, 48 kHz PCM, service lifecycle and safety guards. |
| [`TECHNICAL_INFO.txt`](TECHNICAL_INFO.txt) | Plain-text technical register/driver reference for terminal readers. |
| [`docs/DEVELOPMENT_NOTES.txt`](docs/DEVELOPMENT_NOTES.txt) | Observed hardware tests, known limitations, versioning and development decisions. |
| [`TESTING.md`](TESTING.md) | Installation and physical-hardware acceptance criteria. |
| [`GITHUB_RELEASE.md`](GITHUB_RELEASE.md) | Publishing instructions for the **existing** `nullnode1/essxsof` repository. |

**Register example:** Codec register `0x30` bit `0x20` is used for DAC mute;
`0x33/0x34` control attenuation; `0x4F` provides raw jack flags. On the
**tested notebook only**, raw `0x22` selected speakers and `0x20` selected
headphones. These are **observations, not universal ES8336 defaults**. The
complete masked-write map is in the register notebook. See the corresponding
C source for authoritative implementation details.

## Supported

- Internal speakers (stereo output)
- Wired 3.5 mm headphones (stereo output)
- Automatic jack detection and physical speaker muting, on the tested board
- Separate *logical* PulseAudio speaker/headphone volume profiles
- 48 kHz / 16-bit stereo desktop OSS playback backend
- An external USB microphone is separate from this driver and is not managed by ESSXSOF

**Not supported:** ES8336 headset microphone capture, untested ES8336 boards,
Bluetooth, USB headset routing, and guaranteed suspend/resume support.

## Quick install/upgrade (normal user, not root)

```sh
# Run these from the folder containing the downloaded ZIP:
unzip essxsof-1.0.0.zip
cd essxsof-1.0.0
sh install.sh
```

If you already have the working `0.20.0-rc1` kernel, `install.sh` **does not**
unload or recompile it. It makes a dated backup and safely updates the
**root-owned start scripts**, including a late-boot retry, then updates the
user-session desktop component. Root password is requested for only the
root-owned script update. Close music/video players before installation,
because reconfiguring PulseAudio modules may interrupt ongoing playback.

For a first-time install, `install.sh` compiles against **matching** FreeBSD
kernel sources, installs the native modules using the original guarded
installer, and sets up the desktop session. On hardware other than the
validated model, automatic jack mode remains opt-in in
`/usr/local/etc/essxsof.conf`. No automatic deployment is safe without
confirming the actual hardware design.

## Architecture

```
FreeBSD rc.d early boot -> ESSXSOF kernel PCM playback on /dev/dsp0
           |
           +--> jack monitor: ES8336 input flags -> hardware output_mode
           |
FreeBSD LOGIN stage -> retry jack monitor only if missing (no module reload)
           |
User login -> wait for jack monitor + PulseAudio -> 48 kHz OSS backend
                                                    |          |
                                              Speakers    Headphones
```

Only two named *logical* outputs are made, but the physical `essx_oss_fast`
backend remains visible. Usually you should see exactly **three** outputs;
PulseAudio's automatic `module-oss` detection can sometimes briefly introduce
an additional output. An exact-match cleanup routine removes duplicate
**ES8336 playback-only** OSS outputs, not unrelated capture or USB devices.

## Everyday use

```sh
essxsofctl status
~/.local/bin/essx-desktop status
pactl list short sinks
~/.local/bin/essx-desktop volume speaker 85
~/.local/bin/essx-desktop volume headphone 70
~/.local/bin/essx-desktop cleanup
```

Dry-run cleanup without changes: `essx-desktop cleanup`. To apply:
`essx-desktop cleanup --apply` (requires two profile sinks to be live).
Do not unload PulseAudio modules by numeric ID copied from older sessions.

## Safe rollback / uninstall

Remove **only** the desktop layer, keeping the working kernel:

```sh
sh uninstall-desktop.sh
```

Remove the **complete** ESSXSOF distribution (requires confirmation and root):

```sh
sh uninstall.sh
```

The controller refuses to stop an unidentified live PID or start a second
jack monitor while an ambiguous one may still be running.

The native uninstaller refuses unsafe kernel detach and preserves config,
backups and locally modified files. The boot-maintenance installer saves a
dated backup under `/var/backups/essxsof/` before changing root-owned files.

## Post-reboot acceptance checks

1. Boot once with the analog headset **unplugged**; confirm speakers work.
2. Plug the headset while audio plays: speakers must mute, headphone plays.
3. Unplug while audio plays: playback must return to speakers.
4. Repeat 10 times; sound should not play from both routes simultaneously.
5. Log out and log back in; verify two named outputs + backend are present.
6. Reboot with headset **plugged in** and repeat the transition test.
7. Run `essxsofctl status` and confirm `jack auto routing: running` and fault=0.
8. Check `/var/log/messages` for jack retry warnings and ensure a separate
   USB microphone still exists in `pactl list short sources`.
9. Play 20–30 minutes of continuous audio, with no audible distortion.

**If the jack daemon fails after boot**, avoid force-unloading the kernel:

```sh
su -
/usr/local/sbin/essxsofctl start
/usr/local/sbin/essxsofctl status
exit
~/.local/bin/essx-desktop start
```

**Troubleshooting / diagnostic report** (read-only):

```sh
essxsofctl status
~/.local/bin/essx-desktop doctor
pactl list short sinks
pactl list short sources
grep -Ei 'essxsof|essxcodec|essxsoflate' /var/log/messages | tail -n 60
```

## Technical notes / limits

- Native C/firmware sources and safety checks are copied from 0.20.0-rc1;
  only the startup shell/Python service code and desktop code were revised.
- Built kernel modules must match your running kernel ABI and `/usr/src`.
- No independent hardware cards are created for speaker/headphone: they
  share one ES8336 stereo hardware path.
- This package includes license texts for BSD, SOF and bundled firmware.

See `CHANGELOG.md`, `TESTING.md`, and `src/tests/jackd_offline.py`.

## One-command diagnostics

After reboot, run `sh diagnose.sh` from the extracted directory to collect
**read-only** driver/service/audio status in one terminal output. The diagnostic
includes technical paths and module names but does not capture audio.

### Status checks with a protected PID directory (fixed in RC5)

In RC4, the read-only non-root fallback tried `test -f` on
`/var/run/essxsof/jackd.pid`. That test also fails when the **parent directory**
is restricted to root (0700), so RC4 kept reporting `unknown` even when the
monitor was working. RC5 fixes that specific regression:

- Root still uses the exact root-only PID-file verification for start/stop.
- For normal users **only**, and only with active jack hardware controls,
  status checks the ordinary `ps axww` listing for exactly one standalone
  ESSXSOF jack-monitor process. It does not try to stat the protected file.
- The result says `running (one monitor process visible; PID unverified)`;
  it is a qualified report, not a claim to have read or authenticated the PID.
- If process visibility is restricted, zero or multiple monitors appear,
  or the hardware does not indicate an active monitor, status stays
  conservative (`unknown` or `off`).
- The fallback is **never** used by service start, stop, or unload operations.

Do not change `/var/run/essxsof` or `jackd.pid` permissions. The kernel
and jack routing logic is unchanged from the previously working release.

### Hardware acceptance after installation

The final 1.0.0 optional cleanup fix is new and has not yet been installed on
the target laptop. Check `essxsofctl status`, `essx-desktop status`, and
`sh diagnose.sh` after reboot. Confirm that the jack is routed correctly and
that no microphone/capture source was removed. See `TESTING.md` for the full
acceptance checklist.

## Final 1.0.0 desktop cleanup reliability change

FreeBSD's PulseAudio `module-devd-detect` can reload a duplicate OSS output
while the installer removes it. The manager now revalidates sink/module/capture
ownership before each unload, skips stale candidates, and reports leftovers.
The installer treats this **optional cosmetic cleanup** as a warning rather
than marking an otherwise working audio installation as failed. The physical
kernel driver and routing logic are not changed. Do not disable `module-devd-detect`
or unload other system sound modules to hide these entries.

## Publishing on GitHub

See `GITHUB_RELEASE.md` for a release description, Git commands, known limitations
and a checklist. Do not state universal ES8336 hardware compatibility.
