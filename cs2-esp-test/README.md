# CS2 read-only ESP test project (offline / practice only)

This is a **separate C++ test project** that consumes the output of
[cs2-dumper](https://github.com/Agh-sp/fff) (the `Agh-sp/fff` repository).
It is intended **only for offline / practice CS2**, is **read-only** (no
writes to `cs2.exe`, no DLL injection, no hooks, no anti-cheat bypass), and
shows character positions even behind obstacles because the positions come
from process memory, not from the rendered game image.

**Persian / فارسی:** این یک پروژه آزمایشی C++ جداگانه است که از خروجی
`cs2-dumper` استفاده می‌کند. فقط برای محیط آفلاین/تمرینی CS2؛ فقط-readable،
بدون تزریق، بدون خط-API، بدون دورزدن آنتی چیت، و موقعیت‌ها از طریق حافظه
فرآیند (نه تصویر بازی) نشان داده می‌شود، پس پشت موانع هم قابل‌مشاهده است.

---

## Repository layout

```
cs2-esp-test/
├── README.md          # this file
├── build.ps1          # PowerShell build script (x64, Windows)
├── .gitignore
├── src/
│   ├── mem.h / mem.cpp              # read-only Windows process memory reader
│   ├── offsets.hpp                  # GENERATED: module-relative dw* offsets
│   ├── schema.hpp                   # GENERATED: class field m_* offsets
│   ├── diagnostic.cpp               # Phase 1: memory-reading diagnostic
│   ├── esp_common.h                 # Phase 2: shared entity reading
│   └── esp_display.cpp              # Phase 2: experimental display (radar)
└── tools/
    └── gen_offsets.py               # generates offsets.hpp / schema.hpp from JSON
```

The offset headers are **generated** by `tools/gen_offsets.py` directly from
the dumped `offsets.json` and `client_dll.json` (build 14196) so there is no
manual transcription of values.

---

## Prerequisites (on your Windows machine)

- CS2 must be running (main menu or an offline/practice match suffices).
- A 64-bit C++ compiler, one of:
  - MSVC `cl.exe` (Visual Studio Build Tools, "C++ build tools" workload) —
    run this script from an **x64 Native Tools Command Prompt for VS**, or
  - MinGW `g++.exe` on PATH, or
  - `clang-cl.exe` on PATH.
- Run the terminal **as Administrator** if you get `Access denied` opening
  `cs2.exe` (the game may run elevated).

Drive space: keep the project, build folder and any logs on **F:** (e.g.
clone/copy this repo to `F:\cs2-esp-test` and build there). No absolute
`F:\...` paths are hard-coded in the code; paths are relative.

> ⚠️ Do **not** run this on official matchmaking/competitive servers. Memory
> reading of `cs2.exe` can be flagged by anti-cheat even though this tool is
> read-only. Offline / practice / bots only.

---

## Step-by-step (PowerShell)

Open PowerShell in the `cs2-esp-test` folder (on F:, e.g.
`cd F:\cs2-esp-test`).

### 1. Build the diagnostic tool (Phase 1)

```powershell
.\build.ps1 -Target diagnostic
```

This produces `bin\cs2-esp-diagnostic.exe`.

### 2. Run the diagnostic (cs2.exe must be running)

```powershell
.\bin\cs2-esp-diagnostic.exe
```

What it verifies:
- finds `cs2.exe` and opens it read-only;
- lists loaded modules (`client.dll`, `engine2.dll`, …) with base addresses;
- reads the current game build from `engine2.dll` and **warns if it differs
  from the dump build (14196)** — do not trust offsets if they differ;
- reads every critical global offset (`dwEntityList`, `dwLocalPlayerPawn`,
  `dwLocalPlayerController`, `dwViewMatrix`, …) as an absolute address
  `moduleBase + dwOffset` and reports OK / FAIL;
- resolves the entity list by probing known CS2 layouts and validates every
  entity (team, health, life state, dormant flag, absolute origin);
- reports the local player and the view matrix.

Failed reads are reported as FAIL — they are **never replaced with zero or
guessed**.

### 3. Confirm data, then build the display (Phase 2)

Only after the diagnostic reports the data you expect:

```powershell
.\build.ps1 -Target esp
.\bin\cs2-esp-display.exe
```

This opens a window with a top-down radar of player positions (read-only,
memory-based).

**Keys:**
- `INSERT` = open/close the settings menu (in menu: Up/Down navigate,
  Enter/Space toggle, Insert closes)
- `ESC` = exit (if the menu is open, it closes the menu first)
- `+` / `-` = manual zoom
- `A` = toggle auto-zoom
- `R` = recenter and enable auto-zoom

From the Insert menu you can enable/disable ESP, boxes, health bars, and set
the team filter (All / CT / Terrorist).

### Rebuild after changes

```powershell
.\build.ps1 -Target diagnostic -Clean
.\build.ps1 -Target esp -Clean
```

---

## Offset conventions respected in this project

- **Module-relative offsets (`dw*`)** from `offsets.json` are relative to the
  module base: `absolute = moduleBase + dwOffset`.
- **Class field offsets (`m_*`)** from `client_dll.json` are relative to the
  class instance base: `absolute = instanceBase + m_Offset`.
- The two are **never mixed**. Failed offsets are not zeroed or guessed.

Key fields used (from `client_dll.json`, build 14196):
- `C_BaseEntity::m_pGameSceneNode = 816`, `m_iTeamNum = 999`, `m_iHealth = 844`,
  `m_iMaxHealth = 840`, `m_lifeState = 852`
- `CGameSceneNode::m_vecAbsOrigin = 200`, `m_bDormant = 259`
- `CEntityInstance::m_pEntity = 16`
- `CCSPlayerController::m_hPlayerPawn = 2348`

---

## Test-status convention

In this project's reporting:
- **Confirmed / اجرا شده:** results actually obtained by running the tool on
  your Windows machine with `cs2.exe` live. (Not available in this Linux
  checkout — you must run the diagnostic yourself and confirm.)
- **Unconfirmed / تأیید نشده:** code paths written but not yet executed
  against a live game (e.g. the Phase 2 display until you confirm Phase 1).

When you run the diagnostic, paste its output back and I will proceed to
Phase 2 refinements.
