# Changelog — TradePriceLimits

All notable changes to **TradePriceLimits** are documented here. The topmost section is automatically included in Steam Workshop release notes and the Workshop page description.

## [1.0.18]
- **WRSR `1.2.0.9` Support:** Verified `price_table_aob` (`0xEBA24`, `res_stride = 0x380`) and `dispatch_check_aob` (`0x1FE750`, `desc_type_off = 0x368`) on WRSR `1.2.0.9` and added its explicit PE fingerprint (`TimeDateStamp = 0x6B05A574`, `SizeOfImage = 0xAE9000`) in `common/config/offsets.ini`, bringing supported builds to `1.2.0.9`, `1.2.0.8`, `1.1.1.13`, `1.1.1.9`, and `1.1.1.7`.

## [1.0.17]
- **WRSR `1.1.1.13` Support:** Added verified PE fingerprint (`TimeDateStamp = 0x6A8D4087`, `SizeOfImage = 0xA9D000`) and offsets (`res_stride = 0x340`, `desc_type_off = 0x360`), bringing supported builds to `1.2.0.8`, `1.1.1.13`, `1.1.1.9`, and `1.1.1.7`.
- **Shared Common Signature & Offset Architecture:** Consolidated shared WRSR PE profiles, AOB signatures, and struct helpers into `common/` (`wrsr::game::GameContext` + `common/config/offsets.ini`) with layered mod overrides.

## [1.0.15 - 1.0.16]
- **Documentation & Workshop Examples:** Added practical gameplay examples to `README.md` and the Steam Workshop description covering price-dip/peak gating, negative-price waste imports, RUB/USD arbitrage, cross-border margins, staggered fallback exports, and `Base %` inflation protection.
- **Steam Workshop Description Guard:** Automatically fits recent changelog sections within Steam's 8,000-character description limit during packaging.

## [1.0.14]
- **Hover-Scoped Overlay Input Locking:** Hooked DirectInput8 `GetDeviceState`, `C3D_INPUT` mouse/key exports, and `WndProc` so clicks and scroll-wheel zoom are blocked when hovering or interacting inside the `F8` overlay while map panning/zooming outside the window continues working normally, and keyboard shortcuts lock while typing.
- **Strict Numeric Fields:** Restricted price limit and `Base %` input boxes to digits (`0`-`9`) and `.` only.
- **Global Hotkey Config (`TradePriceLimits.ini`):** Moved `toggle_key` out of per-save sidecar files into `TradePriceLimits.ini` next to `TradePriceLimits.dll` so players and mod managers (e.g. RML `Open plugin INI`) can edit the hotkey before launching the game.
- **Square Workshop Thumbnail & Versioned Descriptions:** Switched `previewimage.png` to `700x700` square format (`WRSR 1.2.0.8`) and included the mod version in `.rml.json` and `workshopconfig.ini` descriptions.

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
