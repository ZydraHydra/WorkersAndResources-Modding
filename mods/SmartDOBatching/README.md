# SmartDOBatching

> **Stop sending nearly-empty trucks across the republic.**

A DLL plugin for *Workers & Resources: Soviet Republic* that prevents Distribution Offices from dispatching vehicles unless a configurable minimum fill threshold is met. Configured via the shared **F9** overlay tab "DO Batching".

---

## Features

| Feature | Detail |
|---|---|
| **Min Truck Fill %** | 0–100 %, default **50%** — road DO vehicles wait for a proper load |
| **Min Train Fill %** | 0–100 %, default **60%** — rail DO vehicles held for larger batches |
| **Exempt Essential Goods** | Food · Meat · Alcohol · Medicine · Chemicals · Coal · Water · Fuel bypass gating so towns never starve or freeze |
| **Prevent Small-Building Deadlock** | Clamps required batch to 85 % of destination storage so a 12 t truck doesn't wait forever for a 4 t grocery store |
| **Quick Presets** | Vanilla 0% · 50% · 75% · 90% |
| **Save Config** | Writes `SmartDOBatching.ini` alongside the DLL |
| **Rejected Dispatch Counter** | Live count of suppressed near-empty dispatches in the F9 tab |

---

## Requirements

- Workers & Resources: Soviet Republic **v1.1.1.x** or **v1.2.0.x** (AOB-matched automatically)
- **TesmioLoader v0.4.5+** *or* **Republic Mod Loader v1.0.1+**

> **Does NOT work with vanilla Steam launch.** A mod loader is required.

---

## Installation

### Option A — TesmioLoader (Recommended)

1. Subscribe on the Steam Workshop.
2. Download & extract [TesmioLoader](https://github.com/MaxLegend/TesmioLoader/releases) into your WRSR game directory.
3. Run `tesmiolauncher.exe`, tick **SmartDOBatching**, and launch WRSR.
4. Press **F9** in-game → open the **DO Batching** tab.

### Option B — Republic Mod Loader (RML)

1. Subscribe to this mod **and** to [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749).
2. Open `RepublicModLoader.exe`, tick **SmartDOBatching**, click **Launch WRSR**.
3. Press **F9** in-game → open the **DO Batching** tab.

### Manual (Non-Steam)

Copy `SmartDOBatching.dll` and `SmartDOBatching.ini` into `<WRSR game dir>\plugins\`.

---

## Configuration

Settings are stored in `SmartDOBatching.ini` next to the DLL and are also editable live via the F9 overlay.

```ini
[settings]
min_truck_fill_pct          = 50
min_train_fill_pct          = 60
exempt_essentials           = true
prevent_small_building_deadlock = true
```

| Key | Type | Default | Description |
|---|---|---|---|
| `min_truck_fill_pct` | int 0–100 | `50` | Minimum truck load % before dispatch |
| `min_train_fill_pct` | int 0–100 | `60` | Minimum train load % before dispatch |
| `exempt_essentials` | bool | `true` | Skip gating for critical survival goods |
| `prevent_small_building_deadlock` | bool | `true` | Cap required batch at 85% of dest storage |

---

## Recommended Settings

| Play Style | Truck % | Train % |
|---|---|---|
| Efficiency-first (long supply lines) | 60 | 70 |
| Balanced (default) | 50 | 60 |
| Responsive (small republic, short hauls) | 25 | 40 |
| Vanilla (no gating) | 0 | 0 |

---

## Safety Notes

- **Save-safe:** Only `SmartDOBatching.ini` is written. WRSR binary saves are never touched.
- **Convoys already dispatched** finish their trip unaffected; only new dispatch evaluations are gated.
- **Essential goods** (food, coal, water, fuel, etc.) are exempt by default so critical supply chains are never disrupted.
- **AOB mismatch / unsupported version:** The hook prints `NOT MATCHED` to the mod log and silently disables itself — the game runs vanilla without crashing.
- **Small-building deadlock:** When enabled, the required batch is automatically capped so a vehicle assigned to a small-capacity destination can still dispatch once cargo is reachable.

---

## Companion Mods

- **[CustomsSpeedControl](https://steamcommunity.com/sharedfiles/filedetails/?id=3845944617)** — Scale Customs House load/unload speeds (0.25×–100×) via the same F9 overlay.
- **[TradePriceLimits](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384)** — Set min-sell / max-buy price rules on border Distribution Offices.

---

## Building from Source

```powershell
cd WorkersAndResources-Modding
.\scripts\build.ps1 -Mod SmartDOBatching -Config Release
```

Output: `build\release\latest\mods\SmartDOBatching\SmartDOBatching.dll`

Deploy to game:
```powershell
.\scripts\deploy.ps1 -Mod SmartDOBatching -Config Release
```

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

---

## License

MIT — see root `LICENSE` file.
