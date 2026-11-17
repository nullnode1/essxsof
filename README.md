# ESSXSOF — FreeBSD ES8336 speaker driver

Experimental driver for **Casper Nirvana NB F500 / TN1 001**, tested on
**FreeBSD 15.1-RELEASE amd64**, Intel Tiger Lake **8086:a0c8 / 1e50:6004**.
It is not a general driver for every ES8336 laptop.

Includes the working native driver **0.19.0-rc1** and desktop audio profile
**0.2.0**: direct 48 kHz playback request, smaller requested buffers,
**100% startup volume**, and a manual **0–200%** range. No full kernel
rebuild or stock kernel patch is required.

## Requirements

Matching `/usr/src/sys`, the FreeBSD base compiler/make, Python 3 and a
running per-user PulseAudio desktop. Root access is requested during setup.
If needed, install Python/PulseAudio with `pkg install python3 pulseaudio`.

## Install

Close players and recording applications. Extract the ZIP, open a terminal
inside the `essxsof` folder and run **as your normal desktop user**:

```sh
sh install.sh
```

Enter the root password when prompted. Setup builds the native modules,
installs the driver, enables boot loading and installs the desktop login
helper. PulseAudio is restarted to reopen the audio device. Playback is
available immediately and starts automatically after reboot/login.

Use **Casper_ES8336_Software_200** as the desktop output. The additional
physical backend entries are part of the audio path. Above 100% is software
gain and may distort loud material; it is not a quality improvement.

## Uninstall

Close audio applications and run from the same folder as your normal user:

```sh
sh uninstall.sh
```

Enter the root password when prompted. This removes the native modules and
desktop helper, disables startup and restores the stock PCI driver binding.
Configuration, license documentation and installation backups are retained;
modified installed files are preserved rather than silently deleted.

## Notes

Speaker playback is tested on the board above. Headphone routing,
suspend/resume, simultaneous HDMI/DP on this PCI function and DSP idle power
optimization are not implemented. The existing USB microphone uses its
separate driver. This archive is complete source; native modules are built
locally for the running kernel. Reinstall after changing the kernel/sources.

The combined host module includes GPL-2.0-only code; other BSD notices and
the separate Intel firmware terms are retained. See `LICENSE` and
`licenses/`. Keep the full source and notices when sharing binaries.
Developed with AI assistance and hardware testing by Görkem Çalışkan.
