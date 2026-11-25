# Contributing

Contributions, bug reports, offset verifications for new WRSR patches, and pull requests are welcome!

> **Project Notice:** This repository is mostly AI-assisted ("vibe coded") and **manually tested and checked**. There is no automated test suite—please test changes carefully in-game on a backed-up save before opening a PR.

## Clean-Room & Hygiene Rules

CI (`.github/workflows/ci.yml`) enforces repository hygiene on every push and pull request:
- **Clean-room loader boundary:** Only use our own C ABI declarations in `common/include/wrsr/loader_abi.hpp`. Never copy GPL or third-party loader source files into tracked paths.
- **No local paths or secrets:** Never commit API/PAT tokens, Steam credentials, game binaries, `.env.local`, or hardcoded local Windows user paths (`C:\Users\...`) in tracked files (CI fails if matched).
- **Valid manifests:** All `*.rml.json` and `game_versions.json` files must be valid JSON.

## Development Setup (Windows x64)

1. Install prerequisites:
   - Visual Studio 2022 (or Build Tools 2022) with *Desktop development with C++*
   - CMake `>= 3.24`, Ninja, and Git
2. Copy `.env.example` to `.env.local` (gitignored) and set your local `SOVIET_DIR` and `RML_PLUGINS_DIR`.
3. Build all mods (`Release` x64, static `/MT` CRT):
   ```powershell
   .\scripts\build.ps1 -Mod all -Config Release -GameVersion all
   ```
4. Deploy locally for testing:
   ```powershell
   .\scripts\deploy.ps1 -Mod TradePriceLimits
   ```

## Coding & Formatting Standards

- **C++20 with static CRT (`/MT`)** so Workshop users never need extra VCRedist DLLs.
- Format C/C++ changes with `.clang-format` (4-space indent, 120-column limit, `Allman` function braces).
- Follow `.gitattributes` line endings (`LF` for C/C++, CMake, Markdown, YAML, JSON, INI; `CRLF` for `.ps1`).
- Put shared WRSR PE profiles, AOB signatures, and struct helpers in `common/` (`common/config/offsets.ini`) rather than duplicating them per mod.

## Versioning & Changelogs

- Each mod's `MAJOR.MINOR` base lives in `mods/<Mod>/VERSION`, and its patch number is derived automatically from Git commit counts touching `mods/<Mod>/` or `common/`.
- When preparing a user-facing change, add bullet points under the top `## [X.Y.Z]` heading of `mods/<Mod>/CHANGELOG.md`—the release workflow automatically extracts that section for GitHub Releases and Steam Workshop notes (within Steam's 8,000-character limit).
- If you are adding a brand-new plugin, follow [docs/ADDING_A_MOD.md](docs/ADDING_A_MOD.md) (including registering the mod in root `CMakeLists.txt` and `.github/workflows/release-workshop.yml`).

## Submitting a Pull Request

1. Fork the repository and create a topic branch from `main`.
2. Keep PRs focused (one bug fix, offset update, or feature per PR).
3. Test in-game with `TesmioLoader` and/or `Republic Mod Loader` and note the WRSR version(s) you verified in the PR checklist.
