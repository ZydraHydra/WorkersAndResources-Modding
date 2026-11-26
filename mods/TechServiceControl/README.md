# TechServiceControl

Native x64 C++ plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that lets you customize Technical Services dispatch thresholds at runtime via the unified `F9` overlay.

- **Steam Workshop:** [TechServiceControl (ID `3846052185`)](https://steamcommunity.com/sharedfiles/filedetails/?id=3846052185)
- **GitHub Releases:** [WorkersAndResources-Modding Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases)

## Features

- **Snowplow Dispatch >= %** (`1%`–`90%`, default `20%`): Holds snowplows in the Technical Services office until road snow depth reaches the configured threshold.
- **Garbage Pickup >= %** (`10%`–`95%`, default `65%`): Adjusts the fill threshold for mixed waste (`75%` vanilla) and specific waste fractions (`60%` vanilla).
- **Sewage Pickup >= %** (`10%`–`95%`, default `70%`): Adjusts the unpiped building sewage tank trigger (`70%` vanilla).
- **Water Delivery <= %** (`5%`–`90%`, default `40%`): Adjusts the unpiped building water tank low-level trigger (`30%` vanilla).
- **Quick Presets:** `Vanilla`, `Fuel Saver`, and `Dense City`, plus `TechServiceControl.ini` persistence.
