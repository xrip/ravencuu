# ravencuu — 11-CU unlock for Windows (Raven Ridge 1002:15dd)

GUI tool: a window with three buttons — **UNLOCK**, **INSTALL**, **REMOVE** —
and a log pane. No console window, no batch files; everything also lands in
`ravencuu.log` next to the exe.

At startup a **hardware check runs automatically** (BYOVD drivers → find
`1002:15dd` → subsystem gate `1458:d000` → BAR5/MMIO → register sanity).
The three buttons arm only when the unlock is possible; otherwise they stay
grey and the log explains why. One action runs at a time.

- **UNLOCK** — one pounce now (disable → write CC → raced enable, one retry).
  Check GPU-Z (512 → 704 shaders) or clinfo (8 → 11 CU) afterwards.
- **INSTALL** — register the boot task `ravencuu` (ONSTART, SYSTEM, HIGHEST);
  the task runs UNLOCK headless with 3 retries at every startup. The task
  binds to the exact copy of `ravencuu.exe` that was run, so the exe must
  live on a boot-mounted disk (e.g. `C:\RavenCU`), not on a USB stick.
- **REMOVE** — delete the boot task (next boot is stock 8 CU).

## How UNLOCK works

The CC register write only works in the window **after the GPU firmware is
loaded, before the driver consumes the CU bitmap**. `pounce` recreates that
window: `pnputil /disable-device` (amdkmdag unloads, display may go black) →
force PCI command MMIO decode → write CC bits 8–10 only →
`pnputil /enable-device` under a `TIME_CRITICAL` spin loop that re-strikes
the single harvest restore (~+284…422 ms) before the bitmap read. Nothing
is flashed; a plain reboot returns the GPU to stock.

## Field log

- **2026-09-17, hardware-verified end-to-end** — clinfo 11 CU / GPU-Z 704
  shaders (Go prototype), then the same sequence via the C port.
- v0.6 (Go): boot task fired before `amdkmdag` left GC reset (BAR5
  all-ones) → fix: wait ≤90 s for sane registers before acting.
- The experiment ladder that mapped the window: pre-OS EFI writes never
  stick; runtime writes with the live driver wedge the machine; offline
  writes are reverted by the fresh init within ~0.5 s; the only valid
  window is inside init — the pounce.
- The Go prototype is retired (kept in the private repo history).

## Package layout

```
ravencuu.exe                  the tool (~20 KB, self-elevating UAC)
drivers\WinRing0x64.sys       PCI config access (service WinRing0_1_2_0)
drivers\ThrottleStop.sys      BAR5 MMIO read/write (service ThrottleStop)
ravencuu.log                  every run appended here
```

Command-line auto-fire (same buttons, run automatically after the check
passes; used by the boot task): `ravencuu.exe pounce` / `unlock` /
`install-autostart` / `uninstall-autostart`. Any other invocation opens
the plain GUI.

## Build

Needs the VS "C++ Build Tools" component; everything else is
self-contained:

```
cd ravenwin\ravencuu && build.cmd     -> ravencuu.exe
```

~20 KB, imports only KERNEL32.dll, SHELL32.dll and USER32.dll — no CRT,
no runtime dependencies. The no-CRT build has its traps — see the header
comment of `ravencuu.c`: raw GUI entries get no argc/argv (parse
`GetCommandLineA`), PE FileAlignment floor is 512, UCRT printf is dead
without CRT startup, MSVC /O1 loop-idiom recognition must be defeated
with `volatile`. GUI traps worth remembering: control ID 0 collides with
the log EDIT's EN_* notifications (buttons use ID base 100), and
multiline EDIT breaks lines only on `\r\n`.

## One-time host preparation

- If the driver service fails to start ("start ThrottleStop failed"):
  Windows Security → Device security → Core isolation details → turn OFF
  **Memory integrity** and the **Microsoft vulnerable driver block list**,
  reboot, retry. Defender may quarantine the two `.sys` files (symptom:
  0-byte file) — add the folder as an exclusion and re-extract.
- Fast Startup OFF (Control Panel → Power Options) so every boot is a
  full GPU reset.

## Provenance

The BYOVD approach (device ioctl codes, service handling) derives from
PZH1gdmu/CMP40HX-Unlock v3.0.0 (MIT, Copyright (c) 2026). Register path
from `uefi/RavenCuTest` / Linux v6.8 gfx_v9 (`raven-gfx9-cu-unlock.patch`).
