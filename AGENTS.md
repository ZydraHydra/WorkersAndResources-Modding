# Repository Agent Guidelines

Read [CONTRIBUTING.md](CONTRIBUTING.md), [docs/ADDING_A_MOD.md](docs/ADDING_A_MOD.md), and [docs/HOOKING_AND_PERFORMANCE_RULES.md](docs/HOOKING_AND_PERFORMANCE_RULES.md) before making changes.

## GRML Integration & Dual-Mode Docking

1. **Host Loader & Canonical ABI:**
   - All mods target **`../GloriousRepublicModLoader` (`GRML`)** via the pure-C ABI header `third_party/grml/grml_api.h` (which must remain an **exact byte-for-byte mirror** of `../GloriousRepublicModLoader/include/grml_api.h`).
   - GRML owns the `GrmlHostApi` / `GrmlUiApi` runtime and passes it into mods via `GrmlPluginInit(const GrmlHostApi* host)`. Mods never force GRML to depend on mod-side code.
2. **Dual-Mode Overlay & Lifecycle Behavior:**
   - **Under GRML:** When `GrmlPluginInit` is invoked, `src/overlay/` docks mod tabs, `WRSR_PROFILE_HOOK` telemetry, AOB pattern scans, and save-lifecycle hooks directly into GRML's `F9` Mod Settings hub without installing duplicate DX11 or `CreateFileW` hooks.
   - **Under Legacy RML / TesmioLoader:** When `GrmlPluginInit` is not called, plugins fall back cleanly to our standalone `src/overlay/` (`Local\WRSR_ModOverlay_<PID>`) implementation.
3. **Hooking & Performance Standards:**
   - Follow [docs/HOOKING_AND_PERFORMANCE_RULES.md](docs/HOOKING_AND_PERFORMANCE_RULES.md) for all simulation, rendering, and file hooks (zero-alloc/lock-free hot paths, `/MT` static CRT, wide paths, SEH guards).

## Build, Test, and Publishing Rules

1. **Local Testing Only (`build.ps1` + `deploy.ps1`):**
   - For local test builds, run only:
     ```powershell
     .\scripts\build.ps1 -Mod <Mod> -Config Release
     .\scripts\deploy.ps1 -Mod <Mod>
     ```
   - **Do NOT** run `package_workshop.ps1` or create `release/*.zip` archives for local test builds.

2. **Publishing & Steam Workshop Safety (Never Touch or Upload Autonomously):**
   - **NEVER** deploy, copy, or modify files inside Steam Workshop (`steamapps\workshop\content\784150\*`) or upload to Steam Workshop without explicit user instruction (`deploy.ps1` targets local GRML only unless `-IncludeWorkshop` is explicitly requested).
   - Steam Workshop uploads and GitHub Releases are handled by **GitHub Actions** ([.github/workflows/release-workshop.yml](.github/workflows/release-workshop.yml)), triggered via `workflow_dispatch` or pushing a `<Mod>-v<SemVer>` tag.
   - **Do NOT** run `.\scripts\publish_workshop.ps1` without `-DryRun` locally. Use `-DryRun` only when verifying staged VDF/description output for a release.
   - When adding a new mod, always register it in **both** root [CMakeLists.txt](CMakeLists.txt) and `.github/workflows/release-workshop.yml` (`inputs.mod.options`).
   - Once GitHub Actions publishes a new mod to Steam Workshop, save the resulting Workshop ID in:
     - `mods/<Mod>/game_versions.json` (`publishedFileId`)
     - `mods/<Mod>/steamcmd/workshopconfig.ini` (`$ITEM_ID`)
     - `mods/<Mod>/README.md` (Steam Workshop links)

3. **Documentation & Workshop Asset Standards:**
   - Match the established style of existing mods (`CustomsSpeedControl`, `TradePriceLimits`, `SmartBayAccess`, `SmartDOBatching`):
     - `README.md`: intro, `> **Alpha Status:**` / `> **Important:**` blockquotes, `Quick Start (For Players New to Mod Loaders)` (TesmioLoader + RML + Manual), `Features & In-Game Usage`, `Practical Examples`, `Controls & INI Configuration`, `Companion Mods`.
     - `steamcmd/preview.json`: generate `steamcmd/previewimage.png` via `.\scripts\generate_preview.ps1 -Mod <Mod>` and visually inspect it.
     - `steamcmd/workshop_sections.json`: render `steamcmd/workshop_item.vdf` via `.\scripts\render_workshop_vdf.ps1 -Mod <Mod>` and keep descriptions under Steam's 8,000-character limit.

4. **CI & Hygiene:**
   - Follow `.github/workflows/ci.yml`: no hardcoded user-profile paths, no secrets, no copyleft loader source outside `common/include/wrsr/loader_abi.hpp`, valid `*.rml.json` and `game_versions.json`, static `/MT` CRT, `.clang-format`, and byte-identical `third_party/grml/grml_api.h`.
