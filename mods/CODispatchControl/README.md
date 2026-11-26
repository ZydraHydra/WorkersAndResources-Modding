# CODispatchControl

Native x64 plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that optimizes Construction Office (CO) vehicle dispatch so COs match truck and bus sizes to actual construction demand instead of sending oversized vehicles for tiny remaining workloads.

## Features

- **Min Material Truck Fill % (`0%`–`100%`, default `50%`)**: Rejects oversized material trucks when `remaining_phase_material / vehicle_capacity` is below the threshold and a smaller capable truck exists in the CO (automatically exempts small final top-offs below `1.5t`).
- **Min Bus Workday Ratio (`0.0`–`2.0`, default `0.5`)**: Prevents dispatching large buses when `remaining_workdays < bus_capacity * ratio` if a smaller bus/microbus exists in the CO.
- **Max Trucks En Route per Site (`1`–`10`, default `2`)**: Caps simultaneous material trucks heading to the same construction site to prevent road-node traffic jams.
- **Mechanism Transport Never Blocked**: Excavators, bulldozers, cranes, and flatbed carrier dispatches pass through untouched.
- **Live F9 Overlay (`CO Dispatch` tab)**: Tune sliders, apply `Vanilla` / `Balanced` / `Strict` presets, and save to `CODispatchControl.ini`.

---

## Installation

### Option A — TesmioLoader (Recommended)

1. Subscribe on the [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3846041311).
2. Download & extract [TesmioLoader](https://github.com/MaxLegend/TesmioLoader/releases) into your WRSR game directory.
3. Run `tesmiolauncher.exe`, tick **CODispatchControl**, and launch WRSR.
4. Press **F9** in-game → open the **CO Dispatch** tab.

### Option B — Republic Mod Loader (RML)

1. Subscribe to this mod **and** to [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749).
2. Open `RepublicModLoader.exe`, tick **CODispatchControl**, click **Launch WRSR**.
3. Press **F9** in-game → open the **CO Dispatch** tab.

### Manual (Non-Steam)

Copy `CODispatchControl.dll` and `CODispatchControl.ini` from `<Steam Library>\steamapps\workshop\content\784150\3846041311\plugins\` (or a GitHub Release zip) into `<WRSR game dir>\plugins\`.

---

## Companion Mods

- [**SmartDOBatching**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845998630) ([GitHub](../../mods/SmartDOBatching)) — Prevent Distribution Offices from dispatching near-empty trucks or trains via the shared **`F9`** overlay (`DO Batching` tab).
- [**CustomsSpeedControl**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845944617) ([GitHub](../../mods/CustomsSpeedControl)) — Adjust Customs House cargo Load and Unload transfer speeds (`0.25x`–`100.0x`, Road & Rail toggle) live via the **`F9`** overlay.
- [**TradePriceLimits**](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384) ([GitHub](../../mods/TradePriceLimits)) — Per-resource Max Buy / Min Sell price thresholds on Customs House dispatches via the **`F8`** overlay.
