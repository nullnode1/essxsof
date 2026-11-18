# ESSXSOF 1.0.0 — GitHub publishing guide

## GitHub release title

ESSXSOF 1.0.0 — FreeBSD 15.1 ES8336 Playback Driver

## Suggested release notes

ESSXSOF is an experimental, playback-only Intel SOF / Everest ES8336 driver for FreeBSD 15.1/amd64, originally developed and tested on a Casper Nirvana NB F500 laptop.

### Features

- Internal speaker and wired 3.5 mm stereo headphone output
- Automatic analog jack detection and physical speaker muting on tested hardware
- Automatic boot-service recovery and desktop session integration
- Separate logical speaker and headphone PulseAudio outputs (plus one technical 48 kHz backend)
- English README, source code, firmware license notices, one-command install/uninstall and read-only diagnostic script

### Important limitations

- This is **NOT** a general ES8336 hardware compatibility claim.
- No headset/internal ES8336 microphone capture, USB/Bluetooth headset support via this driver, or guaranteed suspend/resume behavior.
- The native kernel module still reports 0.20.0-rc1. Distribution is 1.0.0.
- The final distribution's cosmetic cleanup change passed offline tests but requires physical installation acceptance after publishing preparation.
- The installer may update locally installed sound service files; read README before installing and retain a backup.

### Install

```sh
unzip essxsof-1.0.0.zip
cd essxsof-1.0.0
sh install.sh
```

Run `sh diagnose.sh` after a reboot. The ZIP contains complete source and bundled firmware under their respective license texts.

## Publish to the existing GitHub repository

**Repository:** https://github.com/nullnode1/essxsof
**Default branch:** `main` (verified during preparation). The existing
repository already has a README, license, installer, uninstaller and `src/`
tree. **Do not use `git init`, `git push --force`, or delete its history.**

First download/extract the complete release ZIP to the FreeBSD computer.
In a normal user terminal, **clone the existing repo**, then copy the
release files into the working tree without copying any `.git` directory:

```sh
cd ~
git clone https://github.com/nullnode1/essxsof.git essxsof-repo
cp -R "$HOME/İndirilenler/essxsof-1.0.0/." "$HOME/essxsof-repo/"
cd ~/essxsof-repo
git status --short
git diff --stat
git add -A
git diff --cached --stat
git commit -m "Release ESSXSOF 1.0.0 with technical documentation"
git push origin main
```

**Inspect `git diff` before committing.** If the working directory has
unrelated local edits, preserve them instead of overwriting them. If a push
is rejected because the remote changed, fetch and reconcile changes; never
force-push over the public repository history. The `.gitignore` excludes
compiled `*.ko` modules and other build outputs. The signed upstream SOF
firmware and full license texts are part of the source distribution.

To create the GitHub release: open
https://github.com/nullnode1/essxsof/releases/new, choose tag `v1.0.0`,
set title to `ESSXSOF 1.0.0 — FreeBSD ES8336 Playback Driver`, paste the
notes above, attach **the ZIP** as an asset, then publish **only after the
final 1.0.0 on-hardware acceptance checks**.

The GitHub repository is not changed merely by generating this local ZIP.

## Pre-publish acceptance

- [ ] Cold boot with headphones unplugged: speaker works
- [ ] Hotplug 10 times; no simultaneous speaker + headphone audio
- [ ] Cold boot with headphones plugged; automatic routing works
- [ ] Separate speaker/headphone outputs return after login
- [ ] `dev.pcm.0.essx_fault` remains `0`
- [ ] `sh diagnose.sh` has no failed core services
- [ ] Installer exits successfully even if an auto-created duplicate OSS output disappears during cleanup
- [ ] Desktop uninstall restores a basic output, and full uninstall is verified in a **separate disposable test installation** (do not remove your daily driver's working copy just to check)
