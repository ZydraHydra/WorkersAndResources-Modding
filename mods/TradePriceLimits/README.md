# TradePriceLimits

Native x64 C++20 plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that stops Distribution Offices (`Road` and `Rail`) from dispatching new imports above your **Max Buy** cap or new exports below your **Min Sell** floor at Customs Houses.

> **Alpha Status:** Consider this an early **Alpha** release. So far it has only been tested on smaller setups — more testing is needed on large, late-game republics with heavy Distribution Office and rail traffic. Always back up your saves and report any issues on GitHub!

> **Important:** Limits apply to **new** dispatches only — trucks and trains already on their way are never cancelled mid-trip and will finish their current journey.

![TradePriceLimits F8 Overlay](steamcmd/previewimage.png)

## Quick Start (For Players New to Mod Loaders)

Regular Steam Workshop mods only add buildings, maps, or vehicles. Because **TradePriceLimits** adds an in-game `F8` menu and custom trade logic, it runs through a **Mod Loader**. Launching WRSR through Steam's normal **Play** button alone will **not** load `.dll` mods.

Both [**Glorious Republic Mod Loader (GRML)**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846999477) and **TesmioLoader (`v0.4.6+`)** automatically scan your Steam Workshop folder — **no manual DLL copying needed**.

### Option A — Glorious Republic Mod Loader (GRML, Recommended)

1. Subscribe to **TradePriceLimits** on the [Steam Workshop (`WRSR 1.2.0.9, 1.2.0.8, 1.1.1.13, 1.1.1.9 & 1.1.1.7`)](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384) or download the single unified release zip from [GitHub Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases).
2. Subscribe to [**Glorious Republic Mod Loader (GRML)**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846999477) ([GitHub](https://github.com/ZydraHydra/GloriousRepublicModLoader)).
3. Run **`GloriousRepublicModLoader.exe`** from:
   ```text
   <Your Steam Library>\steamapps\workshop\content\784150\3846999477\GloriousRepublicModLoader.exe
   ```
   *(You can create a Desktop shortcut or add it to Steam as a Non-Steam Game.)*
4. Tick **TradePriceLimits** in GRML, click **Launch**, and press **`F8`** in-game!

### Option B — TesmioLoader (`v0.4.6+`, Legacy Alternative)

1. Subscribe to **TradePriceLimits** on the Steam Workshop (links above).
2. Download and extract [**TesmioLoader**](https://github.com/MaxLegend/TesmioLoader/releases) ([Docs](https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started)) into your WRSR game directory.
3. Run **`tesmiolauncher.exe`** — TesmioLoader automatically scans `steamapps\workshop\content\784150\` for compatible plugins (use `workshop_path` or the launcher selector if your Workshop lives in another Steam library).
4. Tick **TradePriceLimits**, launch the game, and press **`F8`** in-game!

*(Manual / Non-Steam installs only: copy `plugins\TradePriceLimits.dll` from `<Steam Library>\steamapps\workshop\content\784150\<ID>\plugins\` or a GitHub Release zip into `<Game Dir>\plugins\`.)*

## Features & In-Game Usage

- **In-Game Overlay (`F8` / `Esc`):** WRSR-themed Dear ImGui panel with cursor capture, search filter, `Active Limits Only` toggle, `? Help` guide, and header tooltips.
- **Separate RUB & USD Limits:** Configure Soviet Customs (`RUB`) and Western Customs (`USD`) independently.
- **Two Modes per Resource:**
  - **Fixed:** Exact RUB and USD limits (`0` = disabled). Example: setting `Max Buy RUB = 45` on `fuel` blocks Soviet fuel imports whenever price exceeds `45 RUB/t`.
  - **Base % (Inflation-Resilient):** Scales limits as a `%` of captured **Baseline** Buy and Sell prices (`Max Buy %` relative to Baseline Buy, `Min Sell %` relative to Baseline Sell). Click **Rebase** (or `Ctrl+R`) to snapshot today's market prices as `100%`.
- **Negative Prices (Trash / Waste):** Signed limits work directly — e.g., `Max Buy RUB = -300` on toxic waste only allows imports when Customs pays you at least `300 RUB/t`.
- **Emergency Bypass (`Enforce Limits`):** Uncheck in the overlay header to pause all blocking immediately without losing your settings.
- **Blocked Checks Readout:** Live counter in the overlay header showing blocked Distribution Office task evaluations since the last Save / Autosave / Load.
- **Save-Safe Sidecar (`trade_price_limits.ini`):** Auto-loads when opening a save and auto-saves on Save / Autosave / `Ctrl+S` inside the save folder without modifying WRSR's binary save files.

## Practical Examples

- **Buy the Dip, Sell the Peak (Price Guard):**
  - **Fuel:** Set `Max Buy RUB = 45` so your import DO only buys Soviet fuel when it is `45 RUB/t` or cheaper.
  - **Steel:** Set `Min Sell USD = 140` so your export DO only sells steel to NATO when they pay `140 USD/t` or more, holding stock until prices recover.
- **Get Paid to Take Waste (Negative Prices):**
  - Customs pays you to import waste (shown as negative prices). Set `Max Buy USD = -280` on `waste_toxic` so your DO **only** imports toxic waste when NATO pays at least `280 USD/t` (e.g. `-290` is allowed; `-200` is blocked). Combine with local waste processing or incineration to profit on both the import payout and recycled materials.
- **Controlled `RUB ↔ USD` Currency Exchange (Arbitrage):**
  - Need `USD` when you have plenty of `RUB` (or vice versa)? Use two DOs with a buffer warehouse between them: **DO 1** imports a good with a strong `USD/RUB` spread (e.g. `clothes` at `Max Buy RUB = 500`), and **DO 2** exports it to NATO (`Min Sell USD = 640`). If market drift or your own volume closes the gap, both DOs pause automatically so you never convert currency at a bad rate.
- **Cross-Border Factory Margin:**
  - Source inputs on the cheaper market and export finished goods to the stronger one—for example, `Max Buy RUB = 135` on `fabric` (buy from Soviets) and `Min Sell USD = 650` on `clothes` (sell to NATO).
- **Priority / Fallback Export Switching (Preferred vs. Backup Border):**
  - `TradePriceLimits` evaluates `RUB` and `USD` independently (it does not compare `RUB` vs. `USD` against each other automatically), but you can still switch export borders automatically using **staggered DO storage thresholds**:
  - Set **DO 1 (Preferred, e.g. NATO)** to export when your warehouse is **> 20% full** with `Min Sell USD = 145`, and **DO 2 (Fallback, e.g. Soviet)** to export when the same warehouse is **> 60% full** with `Min Sell RUB = 115`.
  - While NATO pays `>= 145 USD`, DO 1 keeps the warehouse below 60% so DO 2 stays idle. If NATO prices drop, DO 1 pauses, stock builds to 60%, and DO 2 automatically takes over selling to the Soviets (or both pause if both markets crash).
- **Set-and-Forget with `Base %` (Inflation-Proof):**
  - Press `Ctrl+R` (or click **Rebase**) to snapshot current prices as `100%`, switch a resource to **Base %**, and set e.g. `Max Buy = 95%` / `Min Sell = 105%`. Your limits automatically scale as prices inflate over the decades.

## Companion Mods

- [**CustomsSpeedControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845944617) ([GitHub](../../mods/CustomsSpeedControl)) — Adjust **Customs House (`$TYPE_CUSTOMHOUSE`)** cargo Load and Unload transfer speeds (`0.25x`–`100.0x`, Road & Rail toggle) live via the **`F9`** overlay while keeping domestic logistics at vanilla speed.
- [**SmartDOBatching**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845998630) ([GitHub](../../mods/SmartDOBatching)) — Prevent Distribution Offices from dispatching near-empty trucks or trains via the shared **`F9`** overlay (`DO Batching` tab).
- [**CODispatchControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846041311) ([GitHub](../../mods/CODispatchControl)) — Match Construction Office trucks and buses to site demand and cap simultaneous trucks per site via the shared **`F9`** overlay (`CO Dispatch` tab).
- [**TechServiceControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3846052185) ([GitHub](../../mods/TechServiceControl)) — Customize Technical Services snowplow, garbage, sewage, and water dispatch thresholds via the shared **`F9`** overlay (`Tech Services` tab).

## Controls & Rebinding

| Shortcut / Control | Action |
|---|---|
| `F8` *(default)* | Toggle TradePriceLimits overlay |
| `Esc` | Close overlay |
| `Ctrl+S` | Save rules to active save's `trade_price_limits.ini` |
| `Ctrl+R` | Rebase all resources to current live market prices |

To change the overlay toggle key (including before starting the game via RML's **Open plugin INI**), edit `[settings]` in `TradePriceLimits.ini` next to `TradePriceLimits.dll`:

```ini
[settings]
toggle_key=F8
```

Supported values: `F1`–`F12`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `Tab`, `Tilde`, single letters/digits (`A`–`Z`, `0`–`9`), or a hex Windows virtual-key code (e.g. `0x78`).
