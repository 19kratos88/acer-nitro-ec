# acer-nitro-ec project context

## System and installation
- Hardware: Acer Nitro AN515-57; OS: CachyOS.
- Current kernel: `7.2.4-1-cachyos`; LTS: `6.18.50-1-cachyos-lts`.
- Secure Boot is enabled. DKMS modules are signed with the sbctl Database Key.
- `acer-nitro-ec` version `1.0.0` is installed through DKMS for both kernels.
- Installation, signing, package verification, and hardware test results below
  are user-reported session context; do not assume future system state is unchanged.

## EC discoveries and current encoding
- AN515-57 requires bit `0x10` in EC register `0x03` for manual fan control.
  The driver applies this enable quirk to AN515-57 and AN515-58.
- Preserve unrelated bits using a fresh read-modify-write (`current | 0x10`).
  Never restore a stale saved register byte. Separate EC reads/writes are not
  atomic against other drivers or firmware changing the same register.
- CPU mode register: `0x22`; GPU mode register: `0x21`.
- CPU speed register: `0x37`; GPU speed register: `0x3A`.
- CPU AUTO/MANUAL/TURBO: `0x04` / `0x0C` / `0x08`.
- GPU AUTO/MANUAL/TURBO: `0x10` / `0x30` / `0x20`.
- RPM registers: CPU `0x13`/`0x14`, GPU `0x15`/`0x16`.
  Decode CPU as `(EC[0x14] << 8) | EC[0x13]`, GPU analogously.
- PWM scaling intentionally maps hwmon `0–255` to EC raw `0–100`.
  Read: `raw * 255 / 100`; write: `val * 100 / 255` (integer arithmetic).
- Mode values are shared across current model maps. Lifecycle code must use
  the selected register map and existing constants, not hardcoded addresses.

## Canonical source and packaging
- `./acer-nitro-ec.c` is the canonical driver source. `./Makefile` and
  `./dkms.conf` are the canonical build/configuration files.
- `PKGBUILD` packages those three local files, not the GitHub release archive.
  Package identity remains `acer-nitro-ec-dkms`, version `1.0.0`, release `1`.
- `sha256sums=('SKIP' 'SKIP' 'SKIP')` is intentional for local development.
- Files are staged into `$pkgdir/usr/src/acer-nitro-ec-1.0.0/`; packaging
  substitutes `@PKGVER@` with `1.0.0` in the staged `dkms.conf` only.
- Package installation populates `/usr/src/acer-nitro-ec-1.0.0/`, which is
  DKMS's source. Editing the repository does not automatically update it.
- `src/`, `pkg/`, archives, and module build outputs are generated artifacts,
  not development sources; old nested copies may be stale.
- makepkg package contents were verified byte-for-byte against the canonical
  source. This records a completed check, not a guarantee after future edits.

## Implemented lifecycle behavior
- `nitro_enable_fan_control()` is shared by probe and resume.
- Remove, shutdown, and suspend attempt CPU AUTO and GPU AUTO, even if the
  first write fails. Failures are logged; suspend returns an error on failure.
- Resume freshly rechecks EC `0x03` bit `0x10` for AN515-57/AN515-58.
  It does not restore previous manual mode or PWM settings.
- Linux 7.2 APIs: `void` platform `.remove` and `.shutdown`; ordinary sleepable
  callbacks via `DEFINE_SIMPLE_DEV_PM_OPS()` and `.driver.pm`.
- A mutex serializes control writes, lease handling, and lifecycle transitions.
  `writes_blocked` gates writes during suspend and teardown; lifecycle callbacks
  invalidate leases and drain delayed work before restoring AUTO.
- MANUAL has a five-second lease per fan; successful PWM writes renew that
  fan's lease. Expiry of either lease attempts AUTO for both fans. Failed AUTO
  restoration blocks control writes and is retried by delayed work. TURBO is
  not leased; MANUAL must be explicitly reacquired after fallback.
- Remove/shutdown retry AUTO restoration up to three times. Failed suspend
  restores recovery scheduling; successful resume unblocks control writes.
- A local build on `7.2.4-1-cachyos` previously succeeded with no warnings/errors;
  this is a historical check, not verification of subsequent source edits.

## Userspace controller and desktop integration
- Current project files: `nitro-fan`,
  `nitro-fan-gaming.service`, `nitro-fan-gaming-start`, `nitro-fan-auto`,
  `nitro-fan-gaming.desktop`, `nitro-fan-auto.desktop`, `nitro-fan-toggle`,
  and `nitro-fan-toggle.desktop`. These are development files, not generated
  artifacts. The DKMS package does not package these controller files.
- `nitro-fan` supports `auto`, `gaming`, `max`, and read-only `status`.
  Control commands share `/run/nitro-fan.lock`; cleanup attempts both fans AUTO.
- Gaming uses the hotter of CPU/GPU and applies the same PWM to both fans:
  <55 C -> 178; 55–64 C -> 191; 65–74 C -> 204; 75–84 C -> 217;
  >=85 C -> 230.
- Downward hysteresis is 3 C: a lower step requires temperature strictly below
  the crossed threshold minus 3 C. Upward changes use the normal thresholds.
- Gaming and max refresh PWM every second to renew the kernel MANUAL leases.
  Loss of MANUAL exits with AUTO cleanup; control must be restarted explicitly.
- MAX is explicit PWM 255 and is never selected automatically by gaming.
- `nitro-fan-gaming.service` runs `/usr/local/bin/nitro-fan gaming` as root
  on demand, with no automatic restart or boot-enable section. SIGTERM invokes
  controller cleanup. User-reported functional test: start -> MANUAL; stop -> AUTO.
- Gaming-start and Auto helpers call `/usr/bin/systemctl --system` directly,
  without `pkexec`. The installed PolicyKit rule for
  `org.freedesktop.systemd1.manage-units` allows only start/stop of
  `nitro-fan-gaming.service` for user `branislavb`; these actions no longer
  require a password prompt (user-reported setup).
- KDE Gaming launcher/helper starts the service through PolicyKit; Auto
  launcher/helper stops it and verifies both fans report AUTO, allowing up to
  six seconds for kernel lease fallback. Desktop files target helpers under
  `/usr/local/bin`; repository edits do not update installed copies.
- `nitro-fan-toggle` is installed as `/usr/local/bin/nitro-fan-toggle`.
  It checks service activity: inactive -> gaming via `nitro-fan-gaming-start`;
  active -> AUTO via `nitro-fan-auto`. It reuses the existing helpers without
  duplicating fan-control logic. Both toggle directions were functionally tested.
- The physical Nitro key is detected by keyd as F16 on
  `AT Translated Set 2 keyboard`, id `0001:0001:093d12dc`.
  keyd remaps `f16` -> `f24`; KDE binds F24 to
  `/usr/local/bin/nitro-fan-toggle`.
- Reproducible configuration templates: `extras/keyd/nitro.conf` targets the
  built-in keyboard and maps F16 to F24; `extras/polkit/49-nitro-fan.rules`
  grants only service start/stop to `branislavb`, without generic passwordless
  sudo/root access. Adjust the rule's installation-specific username for another
  user. These project templates are not automatically installed; the KDE F24
  binding remains a separate desktop setting.
- Desktop launchers (Gaming, Auto, Toggle) and terminal commands remain fallback
  controls. Terminal helpers: `/usr/local/bin/nitro-fan-gaming-start`,
  `/usr/local/bin/nitro-fan-auto`, and `/usr/local/bin/nitro-fan-toggle`;
  `nitro-fan status` remains available for read-only status.
- Installed toggle, keyd, KDE, and PolicyKit setup and functional results above
  are user-reported context; repository files do not guarantee installed state.

## Completed hardware tests
- Manual mode works.
- AN515-57 raw EC speed testing (approximate RPM):
  raw 50 -> CPU 3797 / GPU 4477; raw 60 -> CPU 4109 / GPU 4761;
  raw 80 -> CPU 4615 / GPU 5454; raw 90 -> CPU 4838 / GPU 5769;
  raw >=100 -> approximately maximum fan speed.
- `pwm1=pwm2=255`: approximately CPU 5080 RPM / GPU 6120 RPM.
- Remove callback was tested with the new loaded module: after `modprobe -r`,
  both fans physically returned to firmware AUTO.
- After reload: `pwm1_enable=2`, `pwm2_enable=2`; approximately CPU 2100 RPM /
  GPU 2360 RPM.
- Suspend/resume lifecycle behavior was functionally tested with MANUAL/MAX
  active before suspend. Suspend returned both fans to AUTO; after resume,
  both fans remained AUTO. Resume rechecked the EC fan-control feature bit
  (`0x03` bit `0x10`). Previous MANUAL mode/PWM was intentionally not restored.

## Safety rules for future work
- Do not use `ec_sys` concurrently with `acer_nitro_ec`.
- Discover hwmon by `name=acer_nitro_ec`; never hardcode a `hwmonX` number.
- Firmware AUTO is the safe/default state. Do not leave fans in manual mode
  when stopping a test; arrange approved AUTO restoration before testing.
- Do not run `sudo`, DKMS install/remove, `modprobe`, sysfs writes, suspend,
  reboot, or EC writes without explicit user approval for the action.
  Historical tests above are not authorization to repeat them.
- Do not run suspend tests until explicitly requested.
- Inspect diffs before installing kernel module changes.

## Next planned work
1. Further validate the implemented locking and MANUAL lease failsafe when
   explicitly authorized. Auto/gaming/max profiles and desktop integration
   already exist as documented above.
