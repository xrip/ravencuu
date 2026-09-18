# ravencuu — Raven Ridge 11-CU unlock (Windows + Linux)

Unlocks 11 compute units on a Raven Ridge iGPU (PCI 1002:15dd,
subsystem 1458:d000, 2200G reference board). Stock exposes 8 CUs;
three additional ones are physically present but disabled in the
harvest mask. Both tools clear bits 8–10 of
`CC_GC_SHADER_ARRAY_CONFIG`, gated to the tested board. Validated:
**8 → 11 CU, +28% FP32 in clpeak, full benchmark stability.**

Same register, same gating — but different timing problems on each
platform, which is why both exist:

- **Linux** — the kernel `amdgpu` driver re-applies the harvest mask
  at init; the patch writes *during* init, after the firmware loads,
  before the CU bitmap is consumed. That's the only window that sticks.
- **Windows** — `amdkmdag` is a boot-start driver, so pre-OS writes
  don't stick and runtime writes wedge the live driver. The tool
  recreates the init window: disable → write → enable under a
  `TIME_CRITICAL` spin loop that re-strikes the single harvest
  restore (~+284 ms after enable) before the bitmap read.

## Linux install (Ubuntu 6.8.x)

```sh
curl -fsSL https://github.com/xrip/ravencuu/releases/latest/download/install.sh | sudo bash
```

The script is fetched from the latest GitHub release (so the
release pipeline is what you actually run, not the repo HEAD).
Read it first if you prefer: `curl -fsSL <url> | less`, then pipe
to `sudo bash`. From a checkout: `sudo bash install.sh`.

The script:

1. installs build deps and enables `deb-src` (backups kept as
   `*.pre-cu-unlock`)
2. `apt-get source linux-image-unsigned-$(uname -r)` — the exact source
   of the running kernel
3. applies `raven-gfx9-cu-unlock.patch` (embedded in the script, so
   `curl | bash` needs no second fetch)
4. builds only `amdgpu.ko`, installs it to
   `/lib/modules/$(uname -r)/updates/extra/amdgpu.ko`, runs `depmod`
5. writes `/etc/modprobe.d/raven-cu-unlock.conf`
   (`options amdgpu raven_cu_count=11`) and updates the initramfs

```sh
sudo reboot
sudo dmesg | grep 'Raven CU test'    # -> "count 11, mask 0x700"
clinfo | grep -i 'compute units'     # -> 11
```

## Windows install

Drop the kit onto a boot-mounted disk (e.g. `C:\RavenCU`):

```
ravencuu.exe                  the GUI tool (~20 KB, self-elevating)
drivers\WinRing0x64.sys       PCI config access (service WinRing0_1_2_0)
drivers\ThrottleStop.sys      BAR5 MMIO read/write (service ThrottleStop)
```

Double-click `ravencuu.exe` (the `requireAdministrator` UAC manifest
elevates it). The window has three buttons — **UNLOCK**, **INSTALL**,
**REMOVE** — and a log pane. A hardware check runs automatically at
startup; the buttons arm only when the unlock is possible on this
machine (device `1002:15dd`, subsystem `1458:d000`, sane registers).

- **UNLOCK** — one pounce now (disable → write CC → raced enable, one
  retry). Verify: GPU-Z 512 → 704 shaders, clinfo 8 → 11 CU.
- **INSTALL** — register the boot task `ravencuu` (ONSTART, SYSTEM,
  HIGHEST): UNLOCK runs headless with 3 retries at every startup; a lost
  race just means 8 CU for that boot and a note in `ravencuu.log`. The
  task binds to the exact copy of `ravencuu.exe` that was run, so keep
  the exe on a boot-mounted disk.
- **REMOVE** — delete the boot task (next boot is stock 8 CU).

For batch use the same actions auto-fire after the check passes:
`ravencuu.exe pounce | unlock | install-autostart | uninstall-autostart`.

If the driver service fails to start, Windows Security → Device security
→ Core isolation details → turn OFF **Memory integrity** and the
**Microsoft vulnerable driver block list**, reboot, retry.

## Files

```
README.md                       you are here
LICENSE                         MIT
.github/workflows/release.yml   CI: builds Windows exe on tag, uploads release artifacts
install.sh                      Linux one-liner (also built into the release)
raven-gfx9-cu-unlock.patch      the kernel patch (Linux, 30 lines)
ravenwin/                       Windows tool (source only; exe built by CI)
  README.md                     full Windows field log + provenance
  ravencuu/ravencuu.c           single-file C source (GUI), ~20 KB exe
  ravencuu/build.cmd            MSVC build (vswhere + vcvars; CI uses it too)
  drivers/*.sys                 the two pre-signed BYOVD drivers (MIT, upstream)
  .gitignore
```

## Caveats

- **Both tools only clear CC bits 8–10** (mask `0x700 << 16`).
  Clearing bits 11–15 hung the GPU under Linux and is never done.
- **Tested board only** — both tools gate on subsystem 1458:d000 by
  design; they will not activate on other Raven boards.
- **Linux**: validated on kernel 6.8.x; kernel updates replace the
  module (re-run the installer; DKMS packaging is a future improvement).
  Module is unsigned — sign or disable Secure Boot if it enforces.
- **Windows**: BYOVD runtime needs **Memory integrity** and the
  **Microsoft vulnerable driver block list** off. **Fast Startup**
  should be off (every boot should be a full GPU reset). `AMD Driver
  version 31.0.21912.14` confirmed working — older revisions were
  not tested.

## License

MIT — see `LICENSE`.
