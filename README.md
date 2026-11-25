# Workers & Resources: Soviet Republic — C++ Modding Workshop

Native x64 C++20 plugin workspace for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`), targeting both **Republic Mod Loader (RML)** and **TesmioLoader**.

> **Heads Up / Project Notice:**  
> This repository is **mostly vibe coded (AI-assisted)** and **manually tested and checked**. There is no automated test suite, and native memory/API hooks for `SOVIET64.exe` can break or behave unexpectedly across game updates or alongside other mods. Always back up your saves before testing, inspect the code yourself if needed, and use at your own risk.

## Mods

| Mod | Status | Description |
|---|---|---|
| [`TradePriceLimits`](mods/TradePriceLimits) | v1.0.4 (Alpha) | Per-resource Max Buy / Min Sell price thresholds for **new** Customs House dispatches (`RUB` & `USD`; vehicles already en route finish their trip), `F8` WRSR-themed ImGui overlay, `Enforce Limits` bypass, 60s sim-linked counter, and per-save `trade_price_limits.ini` sidecar. ([Workshop 1.1.1.7](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533316) · [Workshop Latest](https://steamcommunity.com/sharedfiles/filedetails/?id=3845533384) · [Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases)) |

## Repository Layout

```text
WorkersAndResources-Modding/
├── .github/workflows/       # CI build + automated GitHub Release / Steam Workshop publish
├── CMakeLists.txt           # Monorepo CMake (shared FetchContent + common + mods/*)
├── common/                  # Shared static library (clean-room ABI, logger, AOB/IAT helpers)
├── mods/
│   └── TradePriceLimits/    # Self-contained mod sources, RML manifest, Workshop VDF, README
├── scripts/
│   ├── build.ps1            # Build one or all mods (Release x64, static /MT CRT)
│   ├── deploy.ps1           # Deploy a built mod into local RML plugins directory
│   ├── package_workshop.ps1 # Stage clean content/plugins/ layout + preview + VDF + zip
│   └── publish_workshop.ps1 # Local build + stage + SteamCMD upload helper
├── docs/
│   ├── ARCHITECTURE.md
│   ├── REVERSE_ENGINEERING.md
│   └── ADDING_A_MOD.md
└── references/              # Gitignored local reference clones (RML, TesmioLoader, etc.)
```

## Prerequisites (Windows x64)

- **Visual Studio 2022** (or Build Tools 2022) with *Desktop development with C++*
- **CMake** (>= 3.24)
- **Ninja**
- **Git** (used by CMake `FetchContent` for MinHook, Dear ImGui, and mINI)

## Versioning & Game-Version Releases

- **Automatic Per-Mod Versioning:** Each mod defines its `MAJOR.MINOR` base in `mods/<Mod>/VERSION` (e.g. `1.0`). The patch number is derived automatically from Git commits touching `mods/<Mod>/` or `common/` (so changes to one mod never bump another mod's version), or overridden by release tags (`<Mod>-v<SemVer>` / `<Mod>-wrsr<GameVer>-v<SemVer>`). CMake embeds the version into the compiled Windows PE `VS_VERSION_INFO` resource, `RmlPluginQuery`/`TsmPluginInit`, and the output `<Mod>.rml.json`.
- **Separate Releases per WRSR Game Version:** Each mod declares its supported game-version tracks in `mods/<Mod>/game_versions.json` (for example `1.1.1.7` and `latest`), each with its own build directory (`build/release/<GameVer>/`), RML `supportedGameVersions` manifest field, GitHub Release archive, and Steam Workshop `publishedFileId`.

## Building

```powershell
# Build all mods for default game version (Release x64, static /MT)
.\scripts\build.ps1

# Build a specific mod for a specific WRSR game version (or 'all' game versions)
.\scripts\build.ps1 -Mod TradePriceLimits -GameVersion 1.1.1.7
.\scripts\build.ps1 -Mod TradePriceLimits -GameVersion all
```

Compiled binaries are output to `build/release/<GameVersion>/mods/<ModName>/`.

## Local Deployment & Testing

1. Copy [`.env.example`](.env.example) to `.env.local` (gitignored) and fill in your local game & RML paths:
   ```env
   SOVIET_DIR=<SteamLibrary>/steamapps/common/SovietRepublic
   RML_PLUGINS_DIR=<SteamLibrary>/steamapps/workshop/content/784150/3787969749/rml/plugins
   ```
2. Deploy and launch via `RepublicModLoader.exe`:
   ```powershell
   .\scripts\deploy.ps1 -Mod TradePriceLimits -GameVersion 1.1.1.7
   ```

## Publishing to Steam Workshop

### Option A — Automated via GitHub Actions (Recommended)

1. Every push / PR runs [`.github/workflows/ci.yml`](.github/workflows/ci.yml) to verify clean-room hygiene, build all mods across all declared game versions (`Release x64`, `/MT`), and validate Workshop staging.
2. Pushing a release tag:
   - `TradePriceLimits-v1.0.1` builds & releases **all** declared game-version tracks (`1.1.1.7`, `latest`, ...).
   - `TradePriceLimits-wrsr1.1.1.7-v1.0.1` builds & releases **only** the `1.1.1.7` track.
   - Or trigger **Release & Steam Workshop Publish** manually via `workflow_dispatch`.
3. To enable automated Steam Workshop uploads on CI, configure repository secrets:
   - `STEAM_USERNAME`
   - `STEAM_CONFIG_VDF` (base64-encoded SteamCMD `config/config.vdf` with cached Steam Guard login — recommended) **or** `STEAM_PASSWORD`
4. Store each game track's Workshop ID in `mods/<Mod>/game_versions.json` (`publishedFileId`) so updates publish cleanly to each game version's separate Workshop item.

### Option B — Local One-Click Publish

```powershell
# Preview staged packages across all game versions without uploading
.\scripts\publish_workshop.ps1 -Mod TradePriceLimits -GameVersion all -DryRun

# Build, stage, and publish all game-version tracks via local steamcmd.exe
.\scripts\publish_workshop.ps1 -Mod TradePriceLimits -GameVersion all -Visibility 2
```

## Optional: `SovietRepublic-MCP` Integration (Live Save & Economy Inspection)

During development and reverse engineering, you can wire up [`burnshall-ui/SovietRepublic-MCP`](https://github.com/burnshall-ui/SovietRepublic-MCP) to inspect live save `stats.ini` economy prices, trade periods, and `header.bin` state via MCP:

```powershell
.\scripts\setup_soviet_mcp.ps1
```

This installs `SovietRepublic-MCP` into an isolated Python `.venv` inside gitignored `references/SovietRepublic-MCP/` and provides [`scripts/run_soviet_mcp.py`](scripts/run_soviet_mcp.py), which reads `SOVIET_DIR` from `.env.local` without copying files into your game directory.

## Clean-Room & Third-Party Dependencies

- `common/include/wrsr/loader_abi.hpp` provides minimal, clean-room C ABI declarations for RML API v1 and TesmioLoader API v4 so plugins build cleanly without bundling external loader sources.
- Third-party libraries (**MinHook**, **Dear ImGui**, **mINI**) are fetched automatically at configure time via CMake `FetchContent` and are not committed to this repository.
- Reference repositories cloned into `references/` and local paths in `.env.local` are excluded via `.gitignore`.
