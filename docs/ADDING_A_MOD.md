# Adding a New Mod to This Workspace

Follow these steps to create a new x64 C++ plugin under `mods/`:

1. **Create the mod directory:**
   ```text
   mods/MyNewMod/
   ├── CMakeLists.txt
   ├── MyNewMod.rml.json
   ├── VERSION                 # Base MAJOR.MINOR (e.g. "1.0"); patch auto-derived from git
   ├── game_versions.json      # Separate release tracks & Workshop IDs per WRSR version
   ├── README.md
   ├── src/
   │   └── main.cpp
   └── steamcmd/
       ├── workshopconfig.ini
       └── workshop_item.vdf
   ```
2. **Wire automatic versioning & shared library** in `mods/MyNewMod/CMakeLists.txt`:
   - Link `wrsr_common` for `wrsr/loader_abi.hpp`, `wrsr/logger.hpp`, and `wrsr/pe_utils.hpp`.
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
