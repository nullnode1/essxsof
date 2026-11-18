# ESSXSOF internals: how playback, boot, routing and cleanup work

**Versioned scope:** ESSXSOF distribution 1.0.0; native module reports
`0.20.0-rc1` intentionally. This document describes the **actual shipped
source**, not a hypothetical universal FreeBSD ES8336 driver.

## 1. What problem does it solve?

On the validated Casper Nirvana NB F500 board, the Tiger Lake controller
needs an Intel SOF DSP firmware/IPC3 audio pipeline and an ES8336 codec
configured over I2C. Simply exposing an HDA codec is not enough to get the
internal amplifier and headphone route working. ESSXSOF supplies a narrowly
guarded FreeBSD kernel PCM playback bridge, a userspace jack-routing service,
and an optional PulseAudio desktop manager.

**Tested system:** FreeBSD 15.1-RELEASE amd64, PCI `8086:a0c8` at
`0000:00:1f.3`, subsystem `1e50:6004`; ACPI `ESSX8336` / `_SUB=70061E50`;
Intel Tiger Lake SOF firmware; GPIO174 speaker amplifier and GPIO173 jack
input. These are **exact-match safety checks** in the driver, not merely
advisory requirements.

## 2. Component map

| File | Responsibility |
| --- | --- |
| `src/essxsof.c` | PCI match/attach/detach, BAR guards, firmware boot, DSP core control, HDA DMA, root-only sysctls, startup/teardown. |
| `src/essx_codec.c` | Codec I2C read/update/readback, saved register state, DAC setup/mute, analog speaker/headphone route, bias trial and rollback. |
| `src/essx_gpio.c` | Guarded GPIO173 IRQ companion, pin/IRQ ownership checks, mask/ack/rearm. |
| `src/essx_ipc.c` | Parse signed firmware image metadata and construct a minimal SOF IPC3 playback topology. |
| `src/essx_pcm.c` | FreeBSD `sound(4)` playback channel, 48 kHz stereo, ring buffer, poll/callout and fault monitoring. |
| `src/essx_core.h`, `src/essx_position.h`, `src/essx_ring.h`, `src/essx_wire.h` | Bounded helper logic, position validation, ring and DMA constants. |
| `src/sbin/essxsofctl` | Root service control, safe PCI binding, firmware/module checks, jack PID checks, status and restoration. |
| `src/sbin/essxsof-jackd` | Observe known jack-status values and request only confirmed output changes. |
| `src/rc.d/essxsof`, `src/rc.d/essxsoflate` | Early boot startup plus late-boot idempotent retry. |
| `desktop/essx-desktop`, `desktop/migrate.py` | Per-user PulseAudio sink/profile creation, startup retry, optional duplicate-OSS cleanup and migration. |
| `install.sh`, `uninstall.sh`, `repair-boot.sh`, `diagnose.sh` | Distribution install, safe removal, startup repair and read-only diagnostics. |

## 3. Audio data flow

```text
Desktop application
    | PulseAudio app stream (mixing and volume control)
    | `ESSXSOF Speakers` / `ESSXSOF Headphones` (virtual sinks)
    |   both depend on ONE `essx_oss_fast` backend
    v
  FreeBSD OSS /dev/dsp0 (48 kHz 16-bit stereo user-visible format)
    |
  `src/essx_pcm.c`  12 x 4096-byte software buffer / DMA ring
    |
  `src/essxsof.c`  Tiger Lake HDA DMA SD7 + SOF IPC3
    |
  Intel DSP pipeline: HOST -> BUFFER -> DAI (SSP0)
    |
  SSP0 serial audio: 48 kHz, 24 valid bits in 32-bit slots
    |
  ES8336 codec DAC -> speaker amp (GPIO174) or headphone jack
```

**Format nuance:** The OSS playback interface is `s16le` stereo at **48 kHz**
and PulseAudio may use float32 internally in its virtual sinks. The SOF
pipeline / I2S serial link is configured for **24 valid bits in 32-bit slots**.
These are different layers; do not advertise end-to-end 24-bit user audio.
There is no simultaneous second hardware DAC or independent analog speaker
and headphone stream.

## 4. Firmware boot sequence (source-level overview)

Source: `src/essxsof.c`, `src/essx_ipc.c`. The code is *not* a general SOF
firmware loader; it uses an exact known Tiger Lake transfer procedure.

1. Verify exact PCI device/subsystem/location, BAR mapping and controller
   signatures. The HDA BAR0 is expected to be **0x4000 bytes**, the DSP BAR4
   **0x100000 bytes**. The known controller version guards in `essx_attach()`
   must pass.
2. Allocate a single contiguous coherent DMA area of **1 MiB**, using the
   offsets in `src/essx_wire.h`: `ESSX_FW_DATA=0x1000`, `ESSX_CAP_BDL=0x82000`,
   `ESSX_OUT_BDL=0x83000`, `ESSX_AUDIO_DATA=0x84000`.
3. Get the firmware module resource named `essx8336_sof`. The bundled
   `src/firmware/sof-tgl.ri` remains Intel-signed upstream material. Image
   parsing validates the `XMan` header, `$CPD` payload, sizes, and bounded
   host/DSP mailbox placement (`essx_parse_image`).
4. Save relevant PCI/power/clock state, reset the HDA engine, enable bus
   mastering, enable processing (`PPCTL`), and safely stop/reset the host-owned
   DSP core 0 only. It does **not** assume ownership of other DSP cores.
5. Configure SD0 as the boot transfer companion, SD7 for the firmware DMA
   transfer. The code uses position-buffer controls (`SPIB7`, `SPIBCTL`) and
   waits for ROM ready/status transitions with strict timeouts.
6. Perform the ROM IPC handshake; wait for the firmware to report
   `FW_READY`, validate the IPC3 major ABI, then build the SOF playback
   topology through explicit request/reply messages.
7. Create the minimal fixed pipeline: `HOST_NEW`, `BUFFER_NEW`, `DAI_NEW`,
   `PIPE_NEW`, `CONNECT_HOST_BUFFER`, `CONNECT_BUFFER_DAI`, `SSP0_CONFIG`,
   `PIPE_COMPLETE`, `PCM_PARAMS`.
8. Prepare the ES8336 analog codec while muted, begin the SD7 audio ring,
   send SOF `STREAM_START`, verify the DMA is advancing, and only then admit
   playback.

### PCM / SSP0 parameters

| Field | Actual configured value | Source |
| --- | --- | --- |
| PCM channels / nominal rate | 2 / 48,000 Hz | `essx_make_topology`, `essx_pcm.c` |
| SOF frame format | `S24_4LE` | `essx_ipc.c` |
| SSP bus | Intel SSP0 (one DAI) | `essx_make_topology()` |
| MCLK / BCLK | 19,200,000 / 3,072,000 Hz | `essx_make_topology()` |
| FSYNC | 48,000 Hz | `essx_make_topology()` |
| Slots | 2 slots, each 32 bits, 24 valid sample bits | `essx_make_topology()` |
| Host period bytes | 4096 | `essx_make_topology()` |
| Audio DMA ring | 49,152 bytes | `ESSX_AUDIO_SIZE` |
| SOF buffer component | 768 bytes | `essx_make_topology()` |
| SOF pipe schedule | 1,000 microseconds; 48 frames/schedule | `essx_make_topology()` |
| PCM direction | Playback-only | `essx_pcm.c` |

A ROM-time use of **SD0/capture-style DMA** is a firmware boot mechanism,
**not** a microphone recording feature. `src/essx_pcm.c` only accepts
`PCMDIR_PLAY`. The native driver provides no ES8336 capture PCM channel.

## 5. HDA and DSP register offsets used in the source

These are **MMIO offsets relative to the mapped BAR**, not physical addresses.
Named meanings below follow the code, not a blanket claim about every Intel
controller. Source: `src/essxsof.c`.

| Domain | Register/offset | Use in this driver |
| --- | --- | --- |
| HDA BAR0 | `0x08` | HDA reset sequence / controller reset completion. |
| HDA BAR0 | `0x20`, `0x4c`, `0x5c` | Disable host interrupt sources during the polling-based initialization. |
| HDA BAR0 | `SD0=0x80` | Boot-time DMA companion stream descriptor. |
| HDA BAR0 | `SD7=0x160` | Firmware transfer and later stereo playback stream descriptor. |
| HDA BAR0 | `PPCTL=0x804` | Processing control; code sets bit 30 before DSP core operations. |
| HDA BAR0 | `SPIBCTL=0x704`, `SPIB7=0x740` | Stream position-in-buffer control for SD7. |
| HDA BAR0 | `0x1030`, `0x1048` | Saved temporary controller power/latency configuration, restored on quiesce. |
| PCI config | `0x44`, `0x48` | Saved/modified clock and power config during firmware boot; restored on quiesce. |
| DSP BAR4 | `ADSPCS=0x04` | DSP core power/reset/status for host-owned core 0. |
| DSP BAR4 | `HIPCTDR=0xc0`, `HIPCTDA=0xc4` | DSP-to-host doorbell and acknowledgement. |
| DSP BAR4 | `HIPCIDR=0xd0`, `HIPCIDA=0xd4` | Host-to-DSP IPC doorbell and acknowledgement. |
| DSP BAR4 | `ROM_STATUS=0x80000` | ROM firmware-transfer state polling. |
| DSP BAR4 | `0x10004 + 0x1000*port` for ports 0,1,2 | Set source-commented ROM SSP clock bits before boot. |

The DMA descriptor layout is written in `setup_stream()`; the buffer
fragment offsets, sizes, status/pointer registers and writeback checks are
not arbitrary free-form configuration. Controller-specific offsets are
**strictly guarded by the PCI and BAR preflight**. They are not safe generic
commands to run against an unknown PCI device.

## 6. Headphone insertion, physical route and desktop policy

The root-owned `essxsof-jackd` follows this observed hardware policy:

```text
Read ES8336 0x4f while the guarded jack detection/bias setup is held
    raw 0x22 -> target route 0 (speakers)
    raw 0x20 -> target route 1 (headphones)
    other    -> do not change route; reset confirmation
Wait for 3 identical valid observations
    |
Use root-only sysctl `dev.essxsof.0.output_mode`
    |
Native codec code mutes DAC, changes analog gain and GPIO174,
waits for settle, then unmutes if successful
    |
Per-user `essx-desktop` notices physical route -> chooses matching
logical PulseAudio sink and restores that sink's saved software volume
```

The monitor's setup orders `jack_poll_prepare`, `jack_detect_enable`,
`jack_micbias_trial`, `jack_bias_playback`, then first `output_mode` write.
It validates a stable boot sample before enabling output. Unknown flag
patterns never force speakers. On teardown it revokes playback admission and
restores bias/detection in reverse order. The IRQ filter does not perform
sleepable I2C or ACPI operations in interrupt context.

## 7. FreeBSD boot, desktop login and why a late retry exists

The native `essxsof` rc.d service starts after base filesystem/module
services and before the normal login stage. The `essxsoflate` service runs
later (requires `LOGIN essxsof`) and makes a **safe idempotent retry** to
start only the userspace jack monitor if the kernel PCM already exists.
It must never detach/reload a running native driver while retrying.

The PulseAudio manager is per-user, not a root-side `pactl` command. It
waits for a usable per-user PulseAudio socket and active native jack state,
sets up a single 48 kHz `essx_oss_fast` OSS backend, then adds two virtual
sink profiles, **ESSXSOF Speakers** and **ESSXSOF Headphones**. It can remove
only exact-match, duplicate playback-only ESSXSOF OSS sinks. A sink that
changes/disappears during cleanup is skipped; this cosmetic cleanup must
not turn an otherwise working install into a fatal failure. **USB microphones
and unrelated recording sources are not cleanup targets.**

The user normally sees **three** sinks (two named profiles plus the physical
backend). The physical backend must remain because both profiles depend on
it. This is a PulseAudio interface design, not three sound cards.

## 8. Safe shutdown and error handling

* `essxsofctl` checks its manifest/build hashes and current kernel ABI to
  reduce accidental loading of incompatible modules.
* Jack PID ownership is verified as root before stop or second start; a
  living but unidentifiable process is **not killed**.
* On stop, codec registers are rolled back, speaker-amplifier GPIO is
  restored, SOF `STREAM_STOP` is sent, DMA and bus mastering are stopped,
  and controller state is restored where possible.
* If the controller cannot establish safe quiescence, it may mark itself
  `poisoned` and refuse detach. **Do not force-unload** the kernel module to
  get rid of a stuck sound device.
* `essx_suspend()` returns `EBUSY`: no working suspend/resume guarantee.
* A user running `essxsofctl status` cannot read the protected PID directory;
  the normal-user fallback is explicitly qualified as
  `running (one monitor process visible; PID unverified)` and is never
  used for service mutation.

## 9. Diagnosing the actual installed driver

From a normal user terminal:

```sh
essxsofctl status
~/.local/bin/essx-desktop status
pactl list short sinks
pactl list short sources
sysctl dev.pcm.0.essx_fault
sh diagnose.sh
```

An `IDLE` or `SUSPENDED` sink is normal if no audio is playing. A value of
`0` in `dev.pcm.0.essx_fault` indicates no currently reported PCM fault;
it does not prove universal compatibility. For genuine sound issues, compare
hardware output mode and jack flags at the time of the event, then inspect
`/var/log/messages`. Reinstalling kernel drivers is not the first response
to a PulseAudio duplicate-sink or PID-reporting issue.

## 10. Limitations, provenance, ownership

* Only one specific board has been verified. The narrow device matching is
  intentional.
* The original internal analog microphone and analog headset microphone are
  **not implemented** as an ESSXSOF capture device. A separate USB input,
  if present, belongs to another driver.
* No complete suspend/resume implementation; HDMI/DP and other unrelated
  sound functions are not part of this project.
* 1.0.0 is a distribution/packaging version. The **native driver remains
  `0.20.0-rc1`** to preserve the hardware-tested kernel logic.
* See `LICENSE`, `licenses/`, `src/firmware/LICENCE.Intel` and the C file
  copyright notices. These documents do not change third-party licenses.
* Development used AI assistance and Linux/Intel upstream sources as
  references; the available field reports were supplied by the maintainer.
  The firmware and external upstream sources are **not original project
  creations**. Never describe the project as an upstream FreeBSD-supported
  driver or claim universal ES8336 support.

Detailed register values, masks, routing and GPIO ownership are in
[`HARDWARE_REGISTERS.md`](HARDWARE_REGISTERS.md).
