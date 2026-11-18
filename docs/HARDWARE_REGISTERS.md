# ES8336 codec register and board-routing notebook

> **ESSXSOF 1.0.0 distribution / native module 0.20.0-rc1**
> Reference implementation: `src/essx_codec.c`, `src/essx_gpio.c`, `src/sbin/essxsof-jackd`.
> Scope: one hardware-tested **Casper Nirvana NB F500 / TN1 001** board, **FreeBSD 15.1-RELEASE amd64**.

This is a **source-audited implementation notebook**, not a complete vendor
ES8336 register datasheet. The constants and masks below are copied from the
shipped source, and the interpretations explicitly noted as *observed* only
apply to the tested machine. Refer to the cited C functions when reviewing a
particular operation. **Do not copy these register writes to other laptops.**

## 1. Physical architecture

```text
             Intel Tiger Lake audio PCI function (8086:a0c8)
             PCI 0000:00:1f.3, subsystem 1e50:6004
                    |
           SOF firmware / IPC3 / SSP0
                    |
          I2S stereo serial audio (48 kHz)
                    |
          ES8336 codec (ACPI ESSX8336)
          I2C device: 7-bit address 0x11
                    |
              DAC / analog output
                 /        \
     Speaker amplifier     3.5 mm analog headphone
     ACPI GPIO174          (same codec, no second sound card)

     Headphone detect: ES8336 status 0x4f + controlled bias
                       + Intel GPIO173 / GPP_D13 IRQ
```

The codec probe checks ACPI HID `ESSX8336`, `_SUB` equal to `70061E50`, and
`iicbus_get_addr(dev) == 0x11`. The I2C transaction structure passes
`.slave=0x22` for writes and the register-address/read sequence. This is the
8-bit bus-address representation used by this code; the *7-bit device
address* remains `0x11`. The codec is not discovered by probing arbitrary
I2C addresses. See `codec_probe`, `codec_read`, and `codec_write`.

### Safety and access model

* The driver obtains **exclusive ownership** of exactly the supported PCI
  function and refuses an unexpected device, subsystem, bus position or BAR
  layout (`essx_probe`, `essx_attach`).
* Attaching the module does **not** immediately execute the codec startup
  sequence. Root enables playback through the guarded service.
* `codec_update(reg, mask, value)` reads the old value, computes
  `(old & ~mask) | (value & mask)`, writes the new value, and verifies it by
  reading it back. The function refuses any register **not** in the explicit
  `codec_regs[]` allowlist.
* The old values of listed registers are saved before startup. Failure and
  shutdown attempt a **reverse-order restore**, amplifier GPIO restoration,
  DMA quiescing, and codec mute. If cleanup is incomplete the driver can
  refuse further operations rather than guess that the hardware is safe.
* The user-facing status/snapshot functions are bounded and read-only. There
  is no public arbitrary I2C register-write feature.

## 2. Codec playback initialization: exact source-level register actions

All values below are **hexadecimal** unless noted. A mask of `ff` replaces
all eight bits. Smaller masks preserve all unrelated bits. Source:
`essx_codec_prepare()` in `src/essx_codec.c`.

| Order | Codec register | Mask | Value (before any mode-dependent changes) | Purpose indicated by the source |
| ---: | :---: | :---: | :---: | --- |
| 1 | `0x30` | `0x20` | `0x20` | **Mute the DAC first**, before other codec modifications. |
| 2 | `0x00` | `0xff` | `0x80` | Enable the code's CSM state; deliberately **avoid** a global `0x3f` reset. |
| 3 | `0x0c` | `0xff` | `0xff` | Part of the validated clock/codec startup sequence; detailed bit meanings are not asserted here. |
| 4 | `0x03` | `0xff` | `0x32` | Paired startup configuration with `0x0c`; see `essx_codec_prepare`. |
| 5 | `0x01` | `0xf4` | `0x74` | MCLK/BCLK input selection and DAC/charge-pump clock configuration in the source comments. |
| 6 | `0x09` | `0xbf` | `0x04` | I2S-related serial-audio configuration, paired with `0x0b`. |
| 7 | `0x0b` | `0x3f` | `0x00` | I2S, 24-bit serial sample configuration. |
| 8 | `0x06` | `0x0f` | `0x01` | Rate/clock-divider configuration paired with `0x07`. |
| 9 | `0x07` | `0xff` | `0x90` | Rate/clock-divider configuration; source notes 19.2 MHz / 48 kHz = 400. |
| 10 | `0x33` | `0xff` | `attenuation_db * 2` | Left/right DAC attenuation register (see note below). |
| 11 | `0x34` | `0xff` | `attenuation_db * 2` | Other DAC attenuation register. |
| 12 | `0x0d` | `0x19` | `0x00` | DAC reference, bias and analog power fields. |
| 13 | `0x14` | `0xcc` | `0x88` | Part of validated analog playback initialization. |
| 14 | `0x15` | `0x33` | `0x00` | Part of validated analog playback initialization. |
| 15 | `0x18` | `0xbb` | `0x11` | Source comment: calibration on, attenuated headphone drive. |
| 16 | `0x19` | `0x04` | `0x00` | Validated analog playback initialization. |
| 17 | `0x1a` | `0x20` | `0x00` | Validated analog playback initialization. |
| 18 | `0x17` | `0x66` | `0x66` | Validated analog playback initialization. |
| 19 | `0x2f` | `0x11` | `0x00` | Validated analog playback initialization. |

The driver pauses after the power/bias setup and again before starting
playback. These are **masked assignments**, not necessarily the final complete
8-bit register values. For undocumented fields, the safest statement is that
the above exact masks/values were used on the validated machine, rather than
inventing a complete datasheet definition.

### DAC attenuation versus desktop volume

`0x33` and `0x34` are assigned together: **two register units per dB**
according to the upstream ES8316-compatible reference used by this driver.
Allowed requested attenuation: `0–48 dB`; with the driver formula, `0 dB`
produces register value `0x00`, and `18 dB` produces `0x24`.

On switching into headphone mode, the native `essx_codec_route()` enforces
**at least 18 dB DAC attenuation** using `MAX(attenuation_db, 18u)`.
This is not the same as a PulseAudio percentage: the desktop `99%` or `100%`
value is a software level on top of the analog playback path. Turning it up
above 100% may clip and does not improve signal quality.

## 3. Routing: how headphones mute the speaker

Source: `essx_codec_route(speaker_level, attenuation_db, headphones)`.
The normal automatic monitor requests only **mode 0** or **mode 1**.

| Mode | Meaning | Register changes / board action |
| --- | --- | --- |
| `output_mode=0` | Internal speakers | Mute DAC via `0x30` bit `0x20`; update `0x33/0x34` using configured attenuation; set `0x18` mask `0x33` to `0x11`; set GPIO174 amplifier to the configured speaker level (normally 1); settle, then unmute DAC. |
| `output_mode=1` | Wired analog headphones | Mute DAC; update `0x33/0x34` with attenuation ≥18 dB; set `0x18` mask `0x33` to `0x00`; set GPIO174 amplifier **0 (off)**; settle, then unmute DAC. |
| `output_mode=2` | Diagnostic only | Headphone-style codec settings with the amplifier allowed on. This could produce **both** speaker and headphone sound; it is **not** the automatic route. |

The route path deliberately mutes before switching the amplifier and DAC
fields, waits about 70 ms, and unmutes afterward. If a switch fails, it tries
to mute again and records an error. The desktop's two sinks are **logical
profiles** feeding the same single hardware PCM backend; choosing a desktop
sink alone is not equivalent to physically disabling the speaker amplifier.

## 4. Jack detection and its exact register policy

Relevant symbols: `JACK_FLAG_REG=0x4f`, `essx_codec_jack_poll_prepare()`,
`essx_codec_jack_detect()`, `essx_codec_micbias_trial()`, and
`src/sbin/essxsof-jackd`.

| Register | Bit/mask | What this project actually does |
| --- | --- | --- |
| `0x4d` | `0x01` | Clears bit 0 to select the **HPInserted input** rather than the alternative ADCLRCK I/O assignment (per source comment). Other bits are preserved. |
| `0x4e` | `0x02` | Initially clears IRQ enable during polling setup; enables this bit only after a guarded host GPIO IRQ handler has been armed. Disables it again on teardown. |
| `0x4f` | read only | Reads raw jack status. The observed exact pair is `0x22` = **speaker/unplugged**, `0x20` = **headphone/plugged**, *on this board under the monitor's held-bias state*. |
| `0x0d` | `0x20` | During a tightly bounded monitor setup, temporarily clears the `pdnMic` bit to enable the existing mic-bias supply for a jack-state comparison. Restores it on shutdown. **Not audio microphone capture.** |
| `0x30` | `0x20` | Mutes the DAC before changing bias/routing, then restores playback only when permitted. |

**Do not generalize the raw `0x4f` values.** They were confirmed for this
specific Casper board; they are **not** an asserted universal ES8336 insertion
bit mapping. An unexpected value does **not** force output to either speaker
or headphone. The monitor requires **three matching consecutive observations**
for a switch, polls at roughly 200 ms intervals, and gives up after a bounded
run of invalid reads. Boot admission also checks a small stable sample window.

The GPIO IRQ serves as a restricted host-side event/acknowledgment mechanism;
the service still reads the codec status to decide the route. An IRQ alone is
not treated as proof of headphone presence.

## 5. Board-specific GPIO and ACPI controls

| Function | Board detail | Driver action |
| --- | --- | --- |
| Speaker amplifier | GPIO**174**, ACPI pad selector `0x0908000e` | Reads ACPI `\_SB.GNUM`, `\_SB.GPC0`, `\_SB.GHPO`, `\_SB.GGPO`; checks pin ownership and mode; writes only level 0/1 via `\_SB.SGOV`. |
| Jack interrupt/input | GPIO**173**, pad selector `0x0908000d`, Intel GPP_D13 | Validates input mode and IRQ ownership; samples but does **not drive** the pin. |
| GPIO companion | ACPI `INT34C5` with the known notebook pad/interrupt resources | Claims only the reviewed IRQ resource, masks only its own enable bit, and acknowledges its own write-one-to-clear status bit. |
| Firmware guard | ACPI `\PCHS == 2` plus GPIO174 checks | Rejects mismatched firmware/board state before speaker-amplifier writes. |

The amplifier guard expects the measured GPIO number `174`, host ownership
`GHPO=1`, `GGPO=0`, and a GPIO output configuration. The jack path checks
GPIO173 as an input and does not modify its direction. These firmware method
names are board-specific; finding similar pin numbers on another laptop is
not sufficient evidence of safety.

## 6. Read-only observations and the meaning of the output

These examples **read** state and do not modify codec registers:

```sh
essxsofctl status
sysctl dev.essxsof.0.driver_version
sysctl dev.essxsof.0.jack_flags
sysctl dev.essxsof.0.output_mode
sysctl dev.essxsof.0.jack_bias_playback
sysctl dev.essxsof.0.codec_snapshot
sysctl dev.pcm.0.essx_fault
```

* `jack_flags: 32` (decimal) is `0x20` (hex) and corresponded to a **plugged**
  headphone in the notebook's real tests.
* `jack_flags: 34` (decimal) is `0x22` and corresponded to **unplugged**.
* `output_mode: 1` means the **headphone route was selected**; `0` means the
  **speaker route was selected**. A state value is not by itself a listening
  test; confirm actual audio at a comfortable level.
* `dev.pcm.0.essx_fault: 0` is a useful check, not a proof of perfect sound
  or guaranteed support for every analog headset.
* `codec_snapshot` returns a **bounded subset** of register reads, not a
  complete register dump. The whitelist is in `essx_codec_snapshot()` and
  includes `00 01 08 0c 0d 13 14 15 16 17 18 19 1a 1b 2f 30 31 33 34 4d 4e 4f`.

## 7. Known limitations and upstream references

* Playback only. No ES8336 analog microphone ADC PCM capture implementation.
* No guaranteed suspend/resume (`essx_suspend()` explicitly returns `EBUSY`).
* No general ES8336 laptop probing or portable register preset.
* Headphone and speakers **share** the ES8336 stereo DAC path; the two
  PulseAudio sinks are a user-interface convenience, not independent DACs.
* This project used *Linux v6.12* `es8316.c` / `es8316.h`, `sof_es8336.c`
  and Intel Tiger Lake pinctrl sources as **references**; the implemented
  FreeBSD I2C/ACPI glue is separate. See copyright headers in the shipped
  `src/essx_codec.c`, `src/essx_gpio.c`, and `licenses/`.
* No independent register-datasheet verification is claimed here for fields
  that the C source does not explain. Exact masks and constants are still
  valuable for reproducibility; unknown bit meanings remain unknown.

For the broader boot/PCM/firmware sequence and source map, see
[`DRIVER_INTERNALS.md`](DRIVER_INTERNALS.md).
