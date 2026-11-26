# Changelog — CustomsSpeedControl

All notable changes to **CustomsSpeedControl** are documented here. The topmost section is automatically included in Steam Workshop release notes and the Workshop page description.

## [1.0.15]
- **Shared `F9` Overlay Update:** Updated `WRSR Tweaks [F9]` overlay hub to hide in-game tweak tabs on the Main Menu until a republic is loaded and support collapsible/live-telemetry ABI extensions (for `MediaRedirector`).

## [1.0.14]
- **Steam Workshop Display Title:** Updated the Steam Workshop listing title to **WRSR Tweaks: Customs Loading Speed (0.25x–100x)** (internal DLL/INI/RML names unchanged).

## [1.0.9]
- **Companion Mod Cross-Links:** Interlinked [`TradePriceLimits`](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384) across `README.md` and the Steam Workshop description.

## [1.0.8]
- **Steam Workshop ID (`3845944617`):** Recorded the published Steam Workshop item ID (`3845944617`) in `game_versions.json`, `README.md`, and the Workshop description.

## [1.0.6]
- **Live Customs House Transfer Hook (`VehicleCargoTransferTick` + `PowerTime`):** Captured and verified unique AOB signature (`cargo_transfer_tick_aob`) across WRSR `1.2.0.9`, `1.2.0.8`, and `1.1.1.13`, hooking `VehicleCargoTransferTick` alongside a `.pdata`-scoped `C3D_TIMER::PowerTime` IAT filter that scales pre-clamp cargo rates (`veh+0x594`: `1` = Unload, `2` = Load) strictly for `$TYPE_CUSTOMHOUSE` (`20`) buildings.
- **WRSR `1.2.0.9` Profile Support:** Added verified `1.2.0.9` fingerprint (`TimeDateStamp = 0x6B05A574`, `SizeOfImage = 0xAE9000`) and versioned `Vehicle` struct offsets (`1.1.1.x` vs `1.2.0.8–1.2.0.9`) to `offsets.ini` and `game_versions.json`.
- **Extended Multiplier Range & Presets:** Raised maximum multiplier cap to `100.0x` (`0.25x`–`100.0x`) and added `25x`, `50x`, and `100x` quick preset buttons to the `F9` overlay.
- **Release Docs & Workshop Assets:** Added `700x700` square `previewimage.png`, comprehensive `README.md`, and Steam Workshop BBCode description in `workshop_item.vdf`.

## [1.0.0]
- **Initial Release Scaffold:** Added `CustomsSpeedControl.dll` (RML API v1 & TesmioLoader API v4 compatible) with `CustomsSpeedControl.ini` persistence (`load_multiplier`, `unload_multiplier`, `affect_rail`).
- **Reusable `F9` Multi-Mod Overlay:** Introduced self-contained `src/overlay/` (`WRSR Tweaks [F9]`) using `Local\WRSR_ModOverlay_<PID>` and a pure-C ABI so multiple plugins share one DX11 + WndProc hook without sharing `ImGuiContext*` or STL types across DLLs.
