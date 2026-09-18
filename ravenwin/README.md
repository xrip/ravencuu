# ravencuu — 11-CU unlock for Windows (Raven Ridge 1002:15dd)

Windows port of the proven Linux `raven-gfx9-cu-unlock.patch` timing.
The CC register write only works in the window **after the GPU firmware
is loaded, before the driver consumes the CU bitmap**. `pounce`
recreates that window on demand:

1. read regs (sanity gates, subsystem check)
2. `pnputil /disable-device` — `amdkmdag` unloads, display may go black
3. force PCI command MMIO decode back on, write CC (bits 8–10 only)
4. `pnputil /enable-device` under a `TIME_CRITICAL` spin loop that
   rewrites CC the instant the fresh init restores the stock harvest mask
   (observed: one restore at +284…422 ms) — before the bitmap is read

Single ~19 KB exe (`ravencuu.exe`), **imports only KERNEL32.dll and
SHELL32.dll** — no CRT, no runtime dependencies. Carries a
`requireAdministrator` UAC manifest (double-click elevates itself; the
boot task and elevated shells see no prompt). Nothing is flashed; a
plain reboot returns the GPU to stock 8 CU.

## Field log

- **2026-09-17, FINAL: hardware-verified end-to-end** — clinfo 11 CU /
  GPU-Z 704 shaders via the Go prototype; the C port (ravencuu)
  then passed the same sequence on the target machine.
- v0.6 (Go): boot-task run fired before `amdkmdag` left GC reset
  (BAR5 all-ones) → fix: wait ≤90 s for sane registers before acting.
- The experiment ladder that mapped the window (all 2026-09-17):
  pre-OS EFI write never sticks; runtime write with live driver wedges
  the machine (Linux documented the same for post-init module writes);
  offline write sticks but the fresh init reverts it within ~0.5 s;
  therefore the only valid window is inside init — the pounce.
- The Go prototype is retired (kept in git history, commit 333decf).

## Package layout

```
ravencuu.exe                  the tool (self-elevating, ~19 KB)
drivers\WinRing0x64.sys       PCI config access (service WinRing0_1_2_0)
drivers\ThrottleStop.sys      physical memory R/W = BAR5 MMIO (service ThrottleStop)
step1-read.cmd                status (read-only)
step2-pounce-11cu.cmd         the unlock (disable -> write -> raced enable)
step3-install-boot.cmd        register the boot task "ravencuu"
step4-remove-boot.cmd         remove the boot task
step5-cleanup.cmd             remove the BYOVD services + driver files
ravencuu.log              every run appended here
```

Build from source (needs the VS "C++ Build Tools" component):

```
cd ravenwin\ravencuu && build.cmd     -> ravencuu.exe
```

The no-CRT build has its traps — see the header comment of `ravencuu.c`:
raw console entries get no argc/argv (parse `GetCommandLineA`), PE
FileAlignment floor is 512, UCRT printf is dead without CRT startup, and
MSVC /O1 loop-idiom recognition must be defeated with `volatile`.

## One-time host preparation

- Run everything from an elevated terminal (or rely on the UAC prompt).
- If the driver service fails to start ("start ThrottleStop failed"):
  Windows Security → Device security → Core isolation details → turn OFF
  **Memory integrity** and the **Microsoft vulnerable driver block list**,
  reboot, retry. Defender may quarantine the two `.sys` files (symptom:
  0-byte file) — add the folder as an exclusion and re-extract the kit.
- Fast Startup OFF (Control Panel → Power Options) so every boot is a
  full GPU reset.
- **The boot task needs the kit on a disk mounted at boot**
  (e.g. `C:\RavenCU`). A USB stick is fine for manual runs only.

## Usage

```
step1:  ravencuu status
        expected stock: CC = 0xFF000000, active CUs = 8.

step2:  ravencuu pounce --count 11 --confirm --retries 3
        clears CC bits 8-10 only (mask 0x700<<16 -> 0xF8000000); bits
        11-15 are never touched (clearing them hung the GPU on Linux).
        Verify afterwards: GPU-Z 512 -> 704 shaders, clinfo 8 -> 11 CU.

step3:  ravencuu install-autostart
        schtasks task "ravencuu" (ONSTART, SYSTEM, HIGHEST) runs
        the pounce headless with 3 retries; output goes to
        ravencuu.log. Brief display blip before login. A lost race
        means 8 CU for that boot and a note in the log.
        The task binds to the exact copy of ravencuu.exe that ran this
        command (GetModuleFileName - any folder, spaces included); that
        folder just has to be mounted at boot, hence "C:\RavenCU".

step4/5: rollback — remove the task BEFORE cleanup (the task re-deploys
        the drivers on its own, so cleanup alone does not disable the
        unlock). A plain reboot is always stock.
```

## Provenance

The BYOVD approach (device ioctl codes, service handling) derives from
PZH1gdmu/CMP40HX-Unlock v3.0.0 (MIT, Copyright (c) 2026). Register path
from `uefi/RavenCuTest` / Linux v6.8 gfx_v9 (`raven-gfx9-cu-unlock.patch`).
