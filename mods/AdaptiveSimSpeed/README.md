# AdaptiveSimSpeed

Native x64 C++20 plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that turns Fast Forward (`>>`) into a hardware-adaptive fast-forward (up to `15.0x`) via the shared **`F9`** multi-mod overlay (`WRSR Tweaks [F9]`), scaling simulation speed dynamically to your available CPU/GPU headroom without tanking FPS in late-game cities.

> **Alpha Status (Early Alpha — Needs More Testing):** Consider this an early **Alpha** release. It hooks into and manipulates many small internal engine/simulation timings (`PreSimUpdate` / `C3D_TIMER`) whose full side effects we do not completely know yet — especially in **large production chains**, dense rail/signal networks, and huge late-game republics. In theory, everything *should* work cleanly because it drives simulation progression within your frame budget rather than breaking `1x` step behavior, **but unexpected side effects may still occur**. Always back up your saves before testing and report any issues on GitHub!

> **Important:** By default (**Only Boost >> (Fast-Forward)** enabled), `1x` Play runs 100% vanilla so building and micro-management remain untouched.

## Quick Start (For Players New to Mod Loaders)

Regular Steam Workshop mods only add buildings, maps, or vehicles. Because **AdaptiveSimSpeed** adds an in-game `F9` overlay and live simulation-speed scaling, it runs through a **Mod Loader**. Launching WRSR through Steam's normal **Play** button alone will **not** load `.dll` mods.

Both **TesmioLoader (`v0.4.5+`)** and **Republic Mod Loader (`v1.0.1`)** automatically scan your Steam Workshop folder — **no manual DLL copying needed**.

### Option A — TesmioLoader (Recommended, `v0.4.0+` Auto Workshop Discovery, tested vs `v0.4.5`)

1. Subscribe to **AdaptiveSimSpeed** on the Steam Workshop or download the unified release zip from [GitHub Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases) (AOB-matched for `WRSR 1.2.0.9`).
2. Download and extract [**TesmioLoader**](https://github.com/MaxLegend/TesmioLoader/releases) ([Docs](https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started)) into your WRSR game directory.
3. Run **`tesmiolauncher.exe`** — since **v0.4.0**, TesmioLoader automatically scans `steamapps\workshop\content\784150\` for compatible plugins (use `workshop_path` or the launcher selector if your Workshop lives in another Steam library).
4. Tick **AdaptiveSimSpeed**, launch the game, and press **`F9`** in-game!

### Option B — Republic Mod Loader (RML `v1.0.1`, All-on-Steam Alternative)

1. Subscribe to **AdaptiveSimSpeed** on the Steam Workshop.
2. Subscribe to [**Republic Mod Loader (RML)**](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) ([GitHub](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader)).
3. Run **`RepublicModLoader.exe`** from:
   ```text
   <Your Steam Library>\steamapps\workshop\content\784150\3787969749\RepublicModLoader.exe
   ```
   *(You can create a Desktop shortcut or add it to Steam as a Non-Steam Game.)*
4. Tick **AdaptiveSimSpeed** in RML, click **Launch**, and press **`F9`** once loaded into your savegame.

*(Manual / Non-Steam installs only: copy `plugins\AdaptiveSimSpeed.dll` and `AdaptiveSimSpeed.ini` from a GitHub Release zip into `<Game Dir>\plugins\`.)*

## Features & In-Game Usage

- **Shared `F9` Multi-Mod Overlay (`WRSR Tweaks [F9]`):** WRSR-themed Dear ImGui hub (`src/overlay/`) with hover-scoped mouse/keyboard locking (`F9` / `Esc` to close). Other utility mods embedding `src/overlay/` automatically appear as tabs in the same window.
- **Headroom-Aware Adaptive Scaling:** Hooks `PreSimUpdate` and measures frame/simulation cost via `QueryPerformanceCounter` (EMA-smoothed), scaling `C3D_TIMER` simulation speed dynamically within your unused frame budget: `(1000 / target_min_fps) - render_reserve_ms`.
- **Configurable Speed Cap (`3.0x` – `15.0x`):** Set **Max Fast-Forward Speed** to cap how fast early/mid-game republics run when you have plenty of FPS headroom.
- **Target Min FPS (`30` – `120`) & Render Reserve (`1.5` – `10.0 ms`):** Protects frame rate and DX11/OS headroom — as your republic grows, the multiplier gracefully scales down instead of tanking FPS.
- **Only Boost >> (Fast-Forward):** Keeps `1x` Play completely vanilla while boosting `>>` Fast Forward.
- **Quick Presets (`Vanilla 3x`, `Balanced 6x`, `Turbo 10x`, `Uncapped 15x`):** One-click buttons to switch speed caps instantly.
- **INI Persistence (`AdaptiveSimSpeed.ini`):** Click **Save Config** (or press `Ctrl+S`) to persist settings beside `AdaptiveSimSpeed.dll` without altering any binary WRSR save files.

## Practical Examples

- **Speed Up Early/Mid-Game Construction:** Set **Max Fast-Forward Speed** to `10x` or `15x` while waiting for starter towns and infrastructure to build, keeping **Target Min FPS = 60**.
- **Smooth Late-Game Fast-Forward:** Leave **Target Min FPS = 60** and **Render Headroom Reserve = 4.0 ms** so fast-forward automatically throttles down in dense cities without stuttering.
- **Vanilla Check:** Click **Vanilla 3x** (or uncheck **Enable Adaptive Fast-Forward**) at any time to revert immediately.

## Controls & INI Configuration

| Shortcut / Control | Action |
|---|---|
| `F9` | Toggle `WRSR Tweaks [F9]` overlay |
| `Esc` | Close overlay |
| `Ctrl+S` | Save settings to `AdaptiveSimSpeed.ini` |
| `Ctrl+Click` slider | Type an exact value |

Default `AdaptiveSimSpeed.ini` alongside `AdaptiveSimSpeed.dll` (editable via RML's **Open plugin INI**):

```ini
[settings]
enabled = true
only_boost_3x = true
max_speed_multiplier = 8.0
target_min_fps = 60
render_reserve_ms = 4.0
```

## Companion Mods

- [**CustomsSpeedControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845944617) ([GitHub](../../mods/CustomsSpeedControl)) — Adjust **Customs House (`$TYPE_CUSTOMHOUSE`)** cargo Load and Unload transfer speeds (`0.25x`–`100.0x`, Road & Rail toggle) live via the **`F9`** overlay.
- [**TradePriceLimits**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384) ([GitHub](../../mods/TradePriceLimits)) — Per-resource Max Buy / Min Sell price thresholds on Customs House dispatches via the **`F8`** overlay.
- [**SmartDOBatching**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845998630) ([GitHub](../../mods/SmartDOBatching)) — Prevent Distribution Offices from dispatching near-empty trucks or trains via the shared **`F9`** overlay (`DO Batching` tab).
- [**CODispatchControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846041311) ([GitHub](../../mods/CODispatchControl)) — Match Construction Office trucks and buses to site demand and cap simultaneous trucks per site via the shared **`F9`** overlay (`CO Dispatch` tab).
- [**TechServiceControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846052185) ([GitHub](../../mods/TechServiceControl)) — Customize Technical Services snowplow, garbage, sewage, and water dispatch thresholds via the shared **`F9`** overlay (`Tech Services` tab).
- [**SmartBayAccess**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846054095) ([GitHub](../../mods/SmartBayAccess)) — Eliminate single-vehicle courtyard bottlenecks at multi-bay road stations via the shared **`F9`** overlay (`Bay Access` tab).
