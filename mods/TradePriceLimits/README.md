# TradePriceLimits

Native x64 C++20 plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that stops Distribution Offices (`Road` and `Rail`) from dispatching new imports above your **Max Buy** cap or new exports below your **Min Sell** floor at Customs Houses.

> **Alpha Status:** Consider this an early **Alpha** release. So far it has only been tested on smaller setups — more testing is needed on large, late-game republics with heavy Distribution Office and rail traffic. Always back up your saves and report any issues on GitHub!

> **Important:** Limits apply to **new** dispatches only — trucks and trains already on their way are never cancelled mid-trip and will finish their current journey.

![TradePriceLimits F8 Overlay](steamcmd/previewimage.png)

## Quick Start (For Players New to Mod Loaders)

Regular Steam Workshop mods only add buildings, maps, or vehicles. Because **TradePriceLimits** adds an in-game `F8` menu and custom trade logic, it runs through a **Mod Loader**. Launching WRSR through Steam's normal **Play** button alone will **not** load `.dll` mods.

### Option A — Republic Mod Loader (RML, Recommended)

1. Subscribe to **TradePriceLimits** on the Steam Workshop ([WRSR 1.1.1.7](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533316) | [WRSR Latest](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384)) or download a release zip from [GitHub Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases).
2. Subscribe to [**Republic Mod Loader (RML)**](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) ([GitHub](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader)).
3. Open RML's Workshop folder:
   ```text
   <Your Steam Library>\steamapps\workshop\content\784150\3787969749\
   ```
4. Run **`RepublicModLoader.exe`** (you can create a Desktop shortcut to it or add it to Steam as a Non-Steam Game).
5. Ensure **TradePriceLimits** is enabled in RML, click **Launch**, and press **`F8`** once loaded into your savegame.

### Option B — TesmioLoader

Also compatible with [**TesmioLoader**](https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started). Copy `TradePriceLimits.dll` into `tesmioloader\build\plugins\` (or install via TLMM) and launch via `tesmiolauncher.exe`.

## Features & In-Game Usage

- **In-Game Overlay (`F8` / `Esc`):** WRSR-themed Dear ImGui panel with cursor capture, search filter, `Active Limits Only` toggle, `? Help` guide, and header tooltips.
- **Separate RUB & USD Limits:** Configure Soviet Customs (`RUB`) and Western Customs (`USD`) independently.
- **Two Modes per Resource:**
  - **Fixed:** Exact RUB and USD limits (`0` = disabled). Example: setting `Max Buy RUB = 45` on `fuel` blocks Soviet fuel imports whenever price exceeds `45 RUB/t`.
  - **Base % (Inflation-Resilient):** Scales limits as a `%` of captured **Baseline** Buy and Sell prices (`Max Buy %` relative to Baseline Buy, `Min Sell %` relative to Baseline Sell). Click **Rebase** (or `Ctrl+R`) to snapshot today's market prices as `100%`.
- **Negative Prices (Trash / Waste):** Signed limits work directly — e.g., `Max Buy RUB = -300` on toxic waste only allows imports when Customs pays you at least `300 RUB/t`.
- **Emergency Bypass (`Enforce Limits`):** Uncheck in the overlay header to pause all blocking immediately without losing your settings.
- **60s Simulation-Linked Counter:** Fixed ring buffer counting blocked customs dispatches over the last 60 seconds of active (unpaused) simulation time.
- **Save-Safe Sidecar (`trade_price_limits.ini`):** Auto-loads when opening a save and auto-saves on Save / Autosave / `Ctrl+S` inside the save folder without modifying WRSR's binary save files.

## Controls & Rebinding

| Shortcut / Control | Action |
|---|---|
| `F8` *(default)* | Toggle TradePriceLimits overlay |
| `Esc` | Close overlay |
| `Ctrl+S` | Save rules to active save's `trade_price_limits.ini` |
| `Ctrl+R` | Rebase all resources to current live market prices |

To change the overlay toggle key, edit `[__settings]` in your save folder's `trade_price_limits.ini`:

```ini
[__settings]
enforcement_enabled=1
toggle_key=F8
```

Supported values: `F1`–`F12`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `Tab`, `Tilde`, single letters/digits (`A`–`Z`, `0`–`9`), or a hex Windows virtual-key code (e.g. `0x78`).
