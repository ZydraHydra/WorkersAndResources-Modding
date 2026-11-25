## Summary
<!-- Briefly describe what this PR changes and why. -->

## Affected Area
- [ ] `TradePriceLimits`
- [ ] `CustomsSpeedControl`
- [ ] `common/` or `src/overlay/`
- [ ] Docs / Scripts / CI

## Checklist
- [ ] **Clean-room compliance:** No third-party GPL or external loader source code added; no secrets, game binaries, or local Windows user paths in tracked files (verified against `.github/workflows/ci.yml`).
- [ ] **Formatting & line endings:** C++ formatted per `.clang-format` and line endings match `.gitattributes` (LF for C++/docs/config, CRLF for `.ps1`).
- [ ] **Build & manifest check:** `.\scripts\build.ps1 -Mod all -Config Release -GameVersion all` succeeds without errors.
- [ ] **Manual in-game testing:** Tested in `SOVIET64.exe` (note WRSR version + RML/TesmioLoader below), since there is no automated test suite.
- [ ] **Docs / metadata updated:** Updated `CHANGELOG.md` (and `CMakeLists.txt` / `release-workshop.yml` / `docs/ADDING_A_MOD.md` if adding a mod).

## Manual Test Notes
- **WRSR Version(s):** 
- **Loader(s):** 
- **What was tested in-game:** 
