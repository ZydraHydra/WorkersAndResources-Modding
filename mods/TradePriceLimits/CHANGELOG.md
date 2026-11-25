# Changelog — TradePriceLimits

All notable changes to **TradePriceLimits** are documented here. The topmost section is automatically included in Steam Workshop release notes and the Workshop page description.

## [1.0.13]
- **Single Unified GitHub & Steam Release:** Removed the duplicate `1.1.1.7` ZIP variant from GitHub Releases so both GitHub Releases and Steam Workshop (`3845533384`) distribute one universal DLL supporting WRSR `1.2.0.8`, `1.1.1.9`, and `1.1.1.7`, with supported game versions clearly listed in the release description.

## [1.0.12]
- **WRSR `1.1.1.9` Profile Support:** Added exact PE fingerprint (`TimeDateStamp = 0x6A3EB6AD`, `SizeOfImage = 0xA9D000`) and verified offsets (`res_stride = 0x340`, `desc_type_off = 0x360`) for WRSR `1.1.1.9` in `offsets.ini`, and added `1.1.1.9` to `supportedGameVersions`.

## [1.0.10]
- **Unified Single-File Offsets (`offsets.ini`):** Consolidated all AOB patterns, PE build fingerprints (`TimeDateStamp` + `SizeOfImage`), and game struct offsets into a single build-embedded configuration file with automatic hotfix operand fallback.
- **Unified Steam Workshop Release:** Combined support for both WRSR `1.2.0.8` and `1.1.1.7` under one auto-updating Steam Workshop item (`3845533384`), while keeping per-version ZIPs on GitHub Releases.
- **Steam Workshop Changenotes:** Automatically publishes release notes to both GitHub Releases and the Steam Workshop description & Change Notes tab.

## [1.0.8 - 1.0.9]
- **WRSR 1.2.0.8 Compatibility Fix:** Updated `EvaluateDistTask` customs detection for shifted `BuildingDescriptor::buildingType` (`0x360` -> `0x368`), fixing negative-price waste/trash import gating on WRSR `1.2.0.8`.
- **Save Folder Binding Fix:** Matched relative save paths (`media_soviet\save\...` without leading slash) in the `CreateFileW` hook so `trade_price_limits.ini` loads and auto-saves reliably on `1.2.0.8`.
- **Reset-on-Save Counter:** Replaced the sim-clock `60s Blocked` ring buffer with a version-proof **`Blocked Checks`** readout that resets cleanly on every Save, Autosave, or Load.
- **Manifest Metadata:** Explicitly listed `1.2.0.8` alongside `1.1.1.7` in `supportedGameVersions`.

## [1.0.4 - 1.0.7]
- **TesmioLoader `v0.4.5` Support:** Exported `TsmPluginTargetVersion = "0.4.5"` and updated installation guides for TesmioLoader's automatic Steam Workshop discovery.
- **Enforce Limits Toggle & Hotkey Rebinding:** Added master `Enforce Limits` checkbox (`[__settings] enforcement_enabled`) and INI-configurable `toggle_key` (default `F8`).
- **UI & Pricing Improvements:** Added separate buy/sell baseline snapshots, collapsible `? Help`, live filter + `Active Limits Only`, signed negative-price support for waste/trash resources, and lock-free `O(1)` dispatch checks.
