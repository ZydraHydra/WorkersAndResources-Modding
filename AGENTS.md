# Repository Agent Guidelines

Read [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/ADDING_A_MOD.md](docs/ADDING_A_MOD.md) before making changes.

## Build, Test, and Publishing Rules

1. **Local Testing Only (`build.ps1` + `deploy.ps1`):**
   - For local test builds, run only:
     ```powershell
     .\scripts\build.ps1 -Mod <Mod> -Config Release
     .\scripts\deploy.ps1 -Mod <Mod>
     ```
   - **Do NOT** run `package_workshop.ps1` or create `release/*.zip` archives for local test builds.

2. **Publishing via GitHub Actions (Never Upload Locally):**
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
   - Follow `.github/workflows/ci.yml`: no hardcoded `C:\Users\...` paths, no secrets, no GPL/third-party loader code outside `common/include/wrsr/loader_abi.hpp`, valid `*.rml.json` and `game_versions.json`, static `/MT` CRT, and `.clang-format`.
