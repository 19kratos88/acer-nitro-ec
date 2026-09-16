# acer-nitro-ec

Linux Embedded Controller (EC) fan control for Acer Nitro laptops, with a
standard hwmon interface, DKMS packaging, and a userspace fan controller.
The current setup focuses on the **Acer Nitro AN515-57 running CachyOS**, with
systemd, KDE Plasma, PolicyKit, and an optional keyd mapping for the Nitro key.

## Supported models and requirements

The driver recognizes AN515-44, AN515-46, AN515-54, AN515-55, AN515-56,
AN515-57, AN515-58, and AN517-55. The functional results documented here are
for AN515-57; recognition does not imply equivalent testing on every model.

Build requirements: matching kernel headers, `make`, and an LLVM toolchain
(`clang`, `llvm-ar`, etc.). The Makefile defaults to `LLVM=1` for kernels such
as CachyOS; use `LLVM=0` for GCC-built kernels. The Arch/CachyOS packaging
workflow also requires makepkg/base-devel and DKMS. Install headers for every
kernel that DKMS should build for.

Userspace requires Bash, `flock` (util-linux), and `awk`; desktop integration
uses systemd and PolicyKit. KDE helpers optionally use `kdialog` for errors
and `notify-send` for notifications. Physical-key integration requires keyd;
`libinput` tools are useful for diagnosing input events.

## Hardware details

AN515-57 requires bit `0x10` in EC register `0x03` for manual fan control.
The driver applies this enable quirk to AN515-57 and AN515-58 using a fresh
read-modify-write (`current | 0x10`) to preserve unrelated bits. Separate EC
reads and writes are not atomic against firmware or other drivers.

| Function | CPU | GPU |
| --- | --- | --- |
| Mode register | `0x22` | `0x21` |
| Speed register | `0x37` | `0x3A` |
| AUTO / MANUAL / TURBO values | `0x04` / `0x0C` / `0x08` | `0x10` / `0x30` / `0x20` |
| RPM low / high bytes | `0x13` / `0x14` | `0x15` / `0x16` |

RPM is `(high << 8) | low`. PWM intentionally maps hwmon **0–255** to EC raw
**0–100**, using integer arithmetic:

- Write: `raw = val * 100 / 255`.
- Read: `val = raw * 255 / 100`.

The nominal read range assumes firmware raw values are at most 100. Reads
are not clamped: a firmware value above 100 produces hwmon PWM above 255.

AN515-57 measurements were approximately CPU/GPU 3797/4477 RPM at raw 50,
4109/4761 at raw 60, 4615/5454 at raw 80, and 4838/5769 at raw 90.
Raw values at or above 100 produced approximately maximum fan speed.
These are observations, not guaranteed RPM targets or percentages of RPM.

## Driver behavior and safety

Firmware **AUTO** is the default/recommended idle mode. hwmon mode values are
`2` = AUTO, `1` = MANUAL, and `0` = TURBO. TURBO is distinct from the userspace
`max` profile, which uses MANUAL with PWM 255.

MANUAL has a **five-second lease per fan**. Successful PWM writes renew that
fan's lease. Successful MANUAL mode writes also start/reset that fan's lease,
including repeated writes of mode 1. Expiry of either lease attempts to restore
**both** fans to AUTO.
Failed AUTO restoration blocks control writes and is retried by delayed work.
TURBO is not leased. MANUAL must be explicitly reacquired after fallback.
A mutex serializes control writes, lease handling, and lifecycle transitions.

Remove, shutdown, and suspend block writes, invalidate leases, drain delayed
work, and attempt AUTO for both fans even if the first write fails. Remove and
shutdown retry up to three times. Suspend reports an error if AUTO restoration
fails and resumes recovery scheduling on the awake device.

Resume rechecks the EC fan-control feature bit. It intentionally does **not**
restore previous MANUAL mode or PWM settings. The reported AN515-57 test began
in MANUAL/MAX: suspend returned both fans to AUTO, and both remained AUTO after
resume. Removal, service start/stop, and both toggle directions were also
functionally tested. These results are historical, as recorded in [AGENTS.md](AGENTS.md).

The kernel lease is the final failsafe if userspace dies, including SIGKILL,
which cannot run shell cleanup. AUTO restoration still depends on working EC
access; inspect errors instead of assuming every restoration succeeded.

## Build and DKMS installation

Run the following examples from the repository root. Installation and control
commands change the system; run them deliberately after reviewing the changes.
Do not use `ec_sys` concurrently with `acer_nitro_ec`.

`acer-nitro-ec.c` is the canonical driver source. `Makefile` and `dkms.conf`
are the canonical build/configuration files. Do not edit generated copies in
`src/`, `pkg/`, or old archives. Inspect diffs before installing kernel changes:

```bash
git diff --check
git diff -- acer-nitro-ec.c Makefile dkms.conf PKGBUILD
make                  # LLVM=1 by default
# Alternative for GCC-built kernels:
# make LLVM=0
```

For Arch/CachyOS, use the current local-source `PKGBUILD`:

```bash
makepkg -s            # Run as your regular user; build the package
sudo pacman -U ./acer-nitro-ec-dkms-1.0.0-1-any.pkg.tar.zst
dkms status
```

The package installs only the canonical driver, Makefile, and DKMS configuration
to `/usr/src/acer-nitro-ec-1.0.0/`. Packaging replaces `@PKGVER@` with `1.0.0`
in the staged `dkms.conf`, leaving the repository template unchanged.
`sha256sums=('SKIP' 'SKIP' 'SKIP')` is intentional for local development;
the package does not download a release archive. DKMS uses the installed
`/usr/src` copy: editing the clone does not update installed sources or an
already loaded module. Rebuild/reinstall after source changes and verify the
DKMS results for each intended kernel.

With Secure Boot enabled, configure DKMS signing with a key trusted by the
machine before loading the module. The recorded setup uses the sbctl Database
Key, with version 1.0.0 installed for `7.2.5-1-cachyos` and
`6.18.50-1-cachyos-lts`. Those are historical local versions, not prerequisites
or a signing configuration supplied by this repository.

After successful installation/signing, load the module if not already loaded:

```bash
sudo modprobe acer-nitro-ec
```

For a non-DKMS manual installation, the Makefile also supports `sudo make install`
(after building, with `LLVM=0` if needed). It runs `modules_install` and `depmod`;
it does not provide DKMS rebuilds or configure Secure Boot signing. Choose one
installation method rather than mixing manual and DKMS copies.

## Install userspace and desktop files

The DKMS package does not install the controller, helpers, service, or launchers.
These commands show their installation destinations:

```bash
sudo install -Dm755 nitro-fan /usr/local/bin/nitro-fan
sudo install -Dm755 nitro-fan-gaming-start /usr/local/bin/nitro-fan-gaming-start
sudo install -Dm755 nitro-fan-auto /usr/local/bin/nitro-fan-auto
sudo install -Dm755 nitro-fan-toggle /usr/local/bin/nitro-fan-toggle
sudo install -Dm644 nitro-fan-gaming.service /etc/systemd/system/nitro-fan-gaming.service
sudo install -Dm644 nitro-fan-gaming.desktop /usr/local/share/applications/nitro-fan-gaming.desktop
sudo install -Dm644 nitro-fan-auto.desktop /usr/local/share/applications/nitro-fan-auto.desktop
sudo install -Dm644 nitro-fan-toggle.desktop /usr/local/share/applications/nitro-fan-toggle.desktop
sudo systemctl daemon-reload
```

Repository edits do not update these installed copies. The service is on-demand;
do **not** enable it at boot. It intentionally has no `[Install]` section.

## nitro-fan userspace tool

```bash
nitro-fan status       # Read-only modes, PWM, RPM, and temperatures
sudo nitro-fan gaming # Foreground dynamic curve; Ctrl+C restores AUTO
sudo nitro-fan auto   # Restore both fans to firmware AUTO
sudo nitro-fan max    # Foreground MANUAL PWM 255; Ctrl+C restores AUTO
```

Run one control command at a time. Controllers share `/run/nitro-fan.lock`;
`status` does not take the lock. Stop a foreground gaming/max process with
Ctrl+C before running `auto`. If gaming is running as a service, use
`nitro-fan-auto` to stop it; `sudo nitro-fan auto` does not stop the service and
will encounter its lock. The Auto helper does not stop an unrelated foreground
controller.

Gaming and max refresh both PWM values on each loop iteration to renew the
leases, even when the target is unchanged. Each iteration sleeps one second
plus time spent processing and accessing the EC; this is not a strict periodic
deadline. Cleanup attempts both AUTO writes on exit,
SIGINT, or SIGTERM. Loss of MANUAL causes an exit with AUTO cleanup; restart
explicitly to regain control. MAX is an explicit manual choice and is never
selected automatically.

## Gaming curve

Gaming uses the **hotter of CPU/GPU**, applying the same target to both fans:

| Temperature | Requested hwmon PWM |
| --- | --- |
| <55 C | 178 |
| 55–64 C | 191 |
| 65–74 C | 204 |
| 75–84 C | 217 |
| >=85 C | 230 |

Upward changes use these thresholds immediately. **3 C downward hysteresis**
requires temperature strictly below the crossed threshold minus 3 C before
reducing a step; for example, the 65 C step drops only below 62 C. Several
steps can be crossed in one update. Gaming never selects PWM 255.

## systemd and KDE integration

`nitro-fan-gaming.service` runs `/usr/local/bin/nitro-fan gaming` as root,
on demand, with `Restart=no`. Stopping sends SIGTERM so shell cleanup restores
AUTO; the unit disables a subsequent SIGKILL. The kernel lease remains the
fallback if PWM refreshes stop.

`Type=simple` start success means systemd launched the service, not that the
controller has already entered MANUAL. The start helper does not verify readiness;
startup failures are reported by the controller in the journal. Use `nitro-fan status`
when confirmation is needed. There are no dependent units requiring a MANUAL-ready
handshake in this repository, so no extra readiness synchronization is used.

| Helper | Behavior |
| --- | --- |
| `nitro-fan-gaming-start` | Start the gaming service |
| `nitro-fan-auto` | Stop the service, then verify both fans report AUTO; allow up to six seconds for lease fallback |
| `nitro-fan-toggle` | Inactive service -> gaming; active service -> AUTO |

Start/stop helpers call `/usr/bin/systemctl --system` directly, without `pkexec`.
The toggle checks both the exit status and text from `systemctl --system is-active`:
only `0:active` selects AUTO and `3:inactive` selects gaming. Query failures,
failed/transitional states, or unexpected responses fail without invoking either
helper. It delegates fan control to the existing helpers without direct sysfs writes.

Toggle invocations for the same desktop user are serialized with a non-blocking
`flock` on `/run/user/$UID/nitro-fan-toggle.lock`, separate from the controller's
`/run/nitro-fan.lock`. The user runtime directory must exist and be owned/writable
by that user; run the toggle as the desktop user. A concurrent invocation exits
harmlessly without acting. The parent holds the lock through helper completion;
helpers/notification children do not inherit it. The lock releases on exit, and
the lock file is retained. This does not serialize different users or independent
start/stop helper calls.

Rapid sequential presses within 750 ms of successful helper completion are
ignored with exit status 0. A separate per-user state file,
`/run/user/$UID/nitro-fan-toggle.last-success`, records boot-relative uptime
(10 ms precision), independent of wall-clock changes. The existing lock protects
both the cooldown check and update. Query/helper failures do not update this
timestamp; helper failures retain their nonzero exit status.

After installing the launchers, KDE offers **Nitro Fans: Gaming**, **Nitro Fans:
Auto**, and **Nitro Fans: Toggle**. Desktop launchers and the helper commands
remain fallback controls when the physical key is unavailable.

## PolicyKit setup

[extras/polkit/49-nitro-fan.rules](extras/polkit/49-nitro-fan.rules) authorizes
only `start` and `stop` of `nitro-fan-gaming.service` through
`org.freedesktop.systemd1.manage-units` for user `branislavb`. Adjust that
installation-specific username before installing for another user.

Example installation, after reviewing/editing the template:

```bash
sudo install -Dm644 extras/polkit/49-nitro-fan.rules /etc/polkit-1/rules.d/49-nitro-fan.rules
```

With the rule active, service start/stop through the helpers no longer requires
a password prompt. This is not generic passwordless sudo/root access and does
not authorize the direct `sudo nitro-fan ...` commands. Keep the installed rule
owned by root and review its exact unit, username, and verbs.

## Physical Nitro key

The tested built-in device is **AT Translated Set 2 keyboard**, keyd id
`0001:0001:093d12dc`. keyd detects the physical Nitro key as **F16**.
[extras/keyd/nitro.conf](extras/keyd/nitro.conf) targets that id and maps
`f16 = f24`; F24 is intentionally the KDE-visible shortcut key. External
keyboards with different ids are unaffected because this is not a wildcard
keyboard configuration.

After installing keyd through your distribution, inspect existing keyd
configuration for conflicting mappings, then install the template:

```bash
sudo install -Dm644 extras/keyd/nitro.conf /etc/keyd/nitro.conf
sudo systemctl enable --now keyd
sudo systemctl restart keyd
```

In KDE System Settings -> Shortcuts, add a command shortcut for
`/usr/local/bin/nitro-fan-toggle` and assign **F24** by pressing the Nitro key
after the keyd mapping is active. The KDE shortcut is a separate per-user
setting; it is not installed by the template. The tested path is:
physical Nitro key -> F16 -> keyd F24 -> KDE -> toggle helper -> service.

## Sysfs interface

The driver exposes these files under `/sys/class/hwmon/hwmonX/`. Discover the
device by `name=acer_nitro_ec`; never hardcode the hwmon number. `nitro-fan`
discovers it automatically and rejects an ambiguous match.

| File | Access | Description |
| --- | --- | --- |
| `fan1_input` | r | CPU fan speed (RPM) |
| `fan2_input` | r | GPU fan speed (RPM) |
| `pwm1` | rw | CPU PWM (0–255) |
| `pwm2` | rw | GPU PWM (0–255) |
| `pwm1_enable` | rw | CPU mode: `0` TURBO, `1` MANUAL, `2` AUTO |
| `pwm2_enable` | rw | GPU mode: `0` TURBO, `1` MANUAL, `2` AUTO |
| `temp1_input` | r | CPU temperature (millidegrees C) |
| `temp2_input` | r | GPU temperature (millidegrees C) |
| `temp3_input` | r | System temperature (millidegrees C) |

Use the lease-aware controller instead of one-shot manual sysfs writes.

## Verification and troubleshooting

```bash
nitro-fan status
systemctl status nitro-fan-gaming.service
journalctl -u nitro-fan-gaming.service
sudo keyd monitor
sudo libinput debug-events --show-keycodes
```

The input monitors help distinguish the physical F16 event from the remapped
F24 event. Stop monitoring with Ctrl+C. An inactive gaming service is normal
in AUTO. If a shortcut does nothing, check the device id/mapping, KDE F24
binding, installed helper paths, and the username/unit/verbs in the PolicyKit
rule. Use the desktop or terminal helpers to isolate shortcut problems.

If hwmon is missing, check module loading, matching kernel headers, `dkms status`,
Secure Boot trust/signing, and kernel logs. After suspend/resume or lease
fallback, expect AUTO and explicitly start gaming again if wanted. Do not
remove the control lock file to bypass an active controller.

PWM readback may differ slightly from the requested value because both
0–255 <-> 0–100 conversions truncate: requesting 178 writes raw 69, which reads
back as 175. This is expected and is not evidence that the curve failed.

Routine PWM write messages use `nitro_dbg` and are debug-only by default;
mode changes, lifecycle messages, and actual errors retain their existing logging.
For driver logging, load with `sudo modprobe acer-nitro-ec debug=1` when the
module is not already loaded, or enable dynamic debug without reloading:

```bash
echo "module acer_nitro_ec +p" | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo dmesg -w | grep acer-nitro-ec
```

Before installing kernel changes, inspect diffs. Never use `ec_sys` alongside
this driver. Arrange AUTO restoration before hardware tests, keep MAX manual-only,
and verify AUTO when stopping control. The lease is a fallback, not a reason
to omit normal cleanup.

## Credits

EC register maps reverse-engineered from the
[Linux-NitroSense](https://github.com/JafarAkhondali/linux-nitroshark) project.
