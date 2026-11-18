# Changelog

## 1.0.0 — playback-only release (2026-11-18)

- Preserve the field-tested FreeBSD 15.1 ES8336 playback-only native kernel module (`0.20.0-rc1` internally).
- Include automatic speaker/headphone routing, reliable late-boot jack monitor retry, PulseAudio login profiles and English documentation.
- Prevent optional duplicate OSS sink cleanup from failing installation when PulseAudio reloads or changes an OSS module concurrently.
- Recheck module, physical sink and recording-source identity immediately before unloading; skip stale candidates without touching unrelated devices.
- Show a warning, rather than fail a working installation, if only optional duplicate-output cleanup cannot finish.
- Retain protected root-owned PID verification and a read-only, qualified status for regular users.
- No ES8336 microphone capture, generic ES8336 laptop support or guaranteed suspend/resume support.
- Final cleanup fix is tested in simulation; physical test is still required on the final distribution.

# Changelog

## 1.0.0-rc5 — protected parent directory status fix (2026-11-18)

- Fix RC4 non-root status regression: `/var/run/essxsof/` is 0700,
  so `test -f /var/run/essxsof/jackd.pid` fails without directory traversal
  permission even when the daemon and its PID file exist.
- On read-only non-root status requests, inspect the process table for exactly
  one standalone monitor while the jack hardware is active. Make the
  qualification explicit: the monitor PID is **not** independently verified.
- Preserve root-only PID verification for all privileged lifecycle actions.
- Preserve 0.20.0-rc1 C sources, DSP firmware, routing, automatic boot retry,
  and PulseAudio desktop behavior without changes.


## 1.0.0-rc4 — safe non-root monitor reporting (2026-11-18)

- Explain protected jack PID files to non-root users without changing permissions.
- When the protected file cannot be read, validate exactly one visible monitor
  process with active jack playback hardware, then show a qualified running state.
- Preserve the original PID-file verification for all privileged start/stop operations.
- Keep FreeBSD 15.1 RC3 hardware behavior and 0.20.0-rc1 kernel source unchanged.
- Treat an ambiguous or invisible process as unknown, never as proven running.


## 1.0.0-rc3 — jack monitor PID verification (2026-11-18)

- Keep the previously validated `0.20.0-rc1` native kernel and codec sources unchanged.
- Fix a FreeBSD-specific false negative in `essxsofctl status`: verify the
  PID from the root-owned PID file against the actual, full `ps axww` process
  listing, matching the exact script argument instead of depending on `ps -o`.
- Refuse to stop a live unidentified PID or spawn a duplicate jack daemon.
- Report a monitor state as `unknown` instead of `off` when hardware jack
  controls are still active but the daemon cannot be authenticated.
- Preserve automatic late boot retry and login-time PulseAudio readiness.
- RC3 requires another on-device reboot test before a stable release.


## 1.0.0-rc2 — boot reliability and consolidated install (2026-11-18)

- Preserve the previously validated native 0.20.0-rc1 C kernel driver.
- Add rc.d late-boot jack monitor retry **after LOGIN** without touching kernel.
- Retry a failed jack-monitor initialization at most three times, only when
  its hardware guard sysctls have returned to their safe inactive state.
- Wait for both physical jack routing and the PulseAudio server during
  desktop auto-login startup (up to 90 seconds).
- Upgrade the existing desktop binary in place rather than incorrectly
  returning without updating when an older version is installed.
- Provide a non-disruptive, backed-up root-owned boot-script maintenance path
  and an English installation/rollback README.
- Keep ES8336 microphone capture explicitly out of scope.

The combined release candidate still needs a fresh cold boot and logged
hardware acceptance on the original laptop before declaring stable.

## Documentation supplement — 2026-11-18

- Added audited ES8336 register and board-routing reference with exact codec masks.
- Documented Tiger Lake BAR/IPC3/firmware/DMA path and FreeBSD PCM behavior.
- Added terminal-friendly technical info and field-test development record.
- Updated the GitHub release guide for the existing `nullnode1/essxsof` repository.
- No kernel C code, firmware, installed system files, or routing algorithms changed by this documentation supplement.
