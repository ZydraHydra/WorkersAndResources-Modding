cmake_minimum_required(VERSION 3.24)
project(WorkersAndResourcesModding LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ModVersioning)
include(EmbedOffsets)

set(WRSR_GAME_VERSION "latest" CACHE STRING "Target WRSR game version track (e.g. 1.1.1.7, latest)")

# CMP0169: allow FetchContent_Populate for MinHook (its upstream CMakeLists targets < 3.5)
cmake_policy(SET CMP0169 OLD)

# Static CRT (/MT) across all targets so subscribers never need VCRedist DLLs
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ── Mod toggles (add new mods here) ───────────────────────────────────────────
option(WRSR_BUILD_TRADE_PRICE_LIMITS "Build TradePriceLimits plugin" ON)
option(WRSR_BUILD_CUSTOMS_SPEED_CONTROL "Build CustomsSpeedControl plugin" ON)

# ── Shared third-party dependencies (fetched once into build/_deps) ───────────
include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    mINI
    GIT_REPOSITORY https://github.com/metayeti/mINI.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(imgui mINI)

FetchContent_Populate(minhook)
FetchContent_GetProperties(minhook SOURCE_DIR minhook_SOURCE_DIR)

# ── Shared MinHook static library ─────────────────────────────────────────────
add_library(minhook_lib STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook_lib PUBLIC ${minhook_SOURCE_DIR}/include)
set_target_properties(minhook_lib PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ── Shared Dear ImGui static library (DX11 + Win32 + Docking) ──────────────────
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(imgui_lib PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ── Header-only mINI interface target ─────────────────────────────────────────
add_library(mini_lib INTERFACE)
target_include_directories(mini_lib INTERFACE ${mINI_SOURCE_DIR}/src)

# ── Shared WRSR plugin foundation ─────────────────────────────────────────────
add_subdirectory(common)

# ── Mods ──────────────────────────────────────────────────────────────────────
if(WRSR_BUILD_TRADE_PRICE_LIMITS)
    add_subdirectory(mods/TradePriceLimits)
endif()
if(WRSR_BUILD_CUSTOMS_SPEED_CONTROL)
    add_subdirectory(mods/CustomsSpeedControl)
endif()
