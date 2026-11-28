# Adding a New Mod to This Workspace

Follow these steps to create a new x64 C++ plugin under `mods/`:

1. **Create the mod directory:**
   ```text
   mods/MyNewMod/
   ├── CMakeLists.txt
   ├── MyNewMod.rml.json
   ├── VERSION                 # Base MAJOR.MINOR (e.g. "1.0"); patch auto-derived from git
   ├── game_versions.json      # Optional visual "workshopTitle", primary "latest" ID + legacy WRSR IDs
   ├── README.md
   ├── src/
   │   └── main.cpp
   └── steamcmd/
       ├── preview.json             # Spec for scripts/generate_preview.ps1 (700x700 suite style)
       ├── workshop_sections.json   # Mod sections for templates/workshop_description.vdf.tpl
       ├── workshopconfig.ini
       └── workshop_item.vdf        # Rendered via scripts/render_workshop_vdf.ps1
   ```
   - **Preview Image Standard**: Define `steamcmd/preview.json` and run `.\scripts\generate_preview.ps1 -Mod MyNewMod` so all mods share the 700×700 Soviet dark-gold-crimson UI mockup style.
   - **Workshop Description Standard**: Define `steamcmd/workshop_sections.json` and run `.\scripts\render_workshop_vdf.ps1 -Mod MyNewMod` to render `steamcmd/workshop_item.vdf` from `templates/workshop_description.vdf.tpl` (shared Alpha notice, GRML + TesmioLoader setup, and changelog footer).
2. **Wire automatic versioning & shared library** in `mods/MyNewMod/CMakeLists.txt`:
   - Link `wrsr_common` for `wrsr/loader_abi.hpp`, `wrsr/logger.hpp`, `wrsr/pe_utils.hpp`, `wrsr/offsets.hpp`, and `wrsr/game_signatures.hpp` (shared WRSR PE profiles, AOB resolvers, `ResourceVector`, and `DistTask` helpers backed by `common/config/offsets.ini`).
   - Optionally call `wrsr_embed_offsets(MyNewMod "${CMAKE_CURRENT_SOURCE_DIR}/config/offsets.ini")` and pass `wrsr::generated::kEmbeddedOffsetsIni` to `wrsr::game::GameContext::initialize(...)` if the mod needs mod-specific signatures or overrides layered on top of `common/config/offsets.ini`.
   - Call `wrsr_configure_mod_version(MyNewMod "${CMAKE_CURRENT_SOURCE_DIR}")` to embed `VS_VERSION_INFO`, define `WRSR_MOD_VERSION` / `WRSR_GAME_VERSION`, and stamp `MyNewMod.rml.json` automatically.
   - Link `minhook_lib`, `imgui_lib`, or `mini_lib` only if needed, keeping `/MT` static runtime.
3. **Register the mod** in the root `CMakeLists.txt` (and add it to the `mod` options list in `.github/workflows/release-workshop.yml`):
   ```cmake
   option(WRSR_BUILD_MY_NEW_MOD "Build MyNewMod plugin" ON)
   if(WRSR_BUILD_MY_NEW_MOD)
       add_subdirectory(mods/MyNewMod)
   endif()
   ```
4. **Build, deploy, stage, and publish:**
   ```powershell
   .\scripts\build.ps1 -Mod MyNewMod -GameVersion all
   .\scripts\deploy.ps1 -Mod MyNewMod
   .\scripts\publish_workshop.ps1 -Mod MyNewMod -GameVersion all -DryRun
   ```
5. **Sync the mod collection:**
   - Run `.\scripts\sync_collection.ps1` to update `workshop_collection.json` and `README.md` (CI checks this automatically).
   - Once the new mod has a `publishedFileId` in `mods/MyNewMod/game_versions.json`, add it to the Steam Workshop Collection on Steam Community (GitHub Actions will remind you in the workflow summary if the live Steam Collection is out of sync).
