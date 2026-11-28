# Workspace Architecture

## Multi-Mod Monorepo Design

This repository hosts multiple independent native x64 C++20 plugins for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`), sharing a common CMake build graph and shared utility library (`common/`).

```text
WorkersAndResources-Modding/
├── CMakeLists.txt            # Root FetchContent (MinHook, ImGui docking, mINI) + targets
├── common/                   # Shared static library (wrsr_common)
│   ├── config/offsets.ini    # Shared WRSR PE profiles, AOB signatures & struct offsets
│   └── include/wrsr/
│       ├── game_signatures.hpp # GameContext runtime PE/AOB resolver & helpers
│       ├── loader_abi.hpp    # Clean-room RML API v1 & Tesmio API v4 C ABI
│       ├── logger.hpp        # Thread-safe log writer next to SOVIET64.exe
│       ├── offsets.hpp       # INI offset profile parser & layered merge
│       └── pe_utils.hpp      # PE section lookup, AOB scanner, IAT patcher
├── src/overlay/              # Shared F9 multi-mod ImGui overlay hub (Pure-C ABI)
│   ├── mod_overlay.h
│   └── mod_overlay.cpp       # Shared via Local\WRSR_ModOverlay_<PID>
└── mods/
    ├── TradePriceLimits/     # Trade price threshold plugin + manifest + Workshop VDF
    └── CustomsSpeedControl/  # Customs House cargo speed plugin + manifest + Workshop VDF
```

### Why a Monorepo?

1. **Single Dependency Cache:** MinHook, Dear ImGui (`docking` branch), and mINI are fetched once via `FetchContent` into `build/` and shared across every mod DLL.
2. **Static CRT (`/MT`):** Every target builds with `/MT` so Steam Workshop subscribers never hit missing Visual C++ Redistributable DLL errors.
3. **Clean-Room Loader Compatibility:** `wrsr/loader_abi.hpp` defines only the binary C ABI contracts (`RmlPluginQuery`/`RmlPluginLoad` and `TsmPluginApiVersion`/`TsmPluginInit`/`TsmPluginStart`) without bundling third-party GPL source files.
4. **Shared Runtime Profiles & `F9` Overlay Hub:** `wrsr_common` embeds `common/config/offsets.ini` (`wrsr::game::GameContext`) so multiple mods share verified PE fingerprints (`1.2.0.9`, `1.2.0.8`, `1.1.1.13`, `1.1.1.9`, `1.1.1.7`) and AOB fallback logic, while `src/overlay/` coordinates one shared DX11 + `WndProc` `WRSR Tweaks [F9]` window across DLLs over a pure-C ABI (`Local\WRSR_ModOverlay_<PID>`).

---

## TradePriceLimits Architecture

```text
TradePriceLimits.dll
├── main.cpp
│   ├── Exports: RmlPluginQuery / RmlPluginLoad (RML v1 — tested vs RML v1.0.1)
│   ├── Exports: TsmPluginApiVersion / TsmPluginInit / TsmPluginStart / TsmPluginTargetVersion (Tesmio API v4, 0.4.6) & GrmlPluginInit (GRML API v2)
│   └── Idempotent worker thread → installs hooks after process init
├── core/rules.{hpp,cpp}
│   ├── RuleEngine singleton (std::mutex for config + lock-free atomic FastThresholdSlot[256] for simulation hot path)
│   ├── Fixed Cap mode (independent max_buy_rub, min_sell_rub, max_buy_usd, min_sell_usd)
│   ├── Base % mode (separate baseline_rub/baseline_sell_rub/baseline_usd/baseline_sell_usd + rebase)
│   ├── Global hotkey loader (toggle_key under [settings]) ← <DllDir>\TradePriceLimits.ini
│   ├── Enforce Limits toggle (persisted per save under [__settings])
│   └── mINI serializer → <SaveDir>\trade_price_limits.ini
├── hooks/dx11_overlay.{hpp,cpp}
│   ├── MinHook vtable hooks on IDXGISwapChain::Present & ResizeBuffers
│   ├── Subclassed WndProc + C3D_INPUT mouse guards for F8 toggle & cursor routing
│   └── WRSR-themed ImGui table with search filter, Active Limits Only, ? Help, and 60s blocked readout
├── hooks/save_tracker.{hpp,cpp}
│   ├── MinHook hook on KERNELBASE!CreateFileW (with KERNEL32 fallback)
│   ├── Matches *\media_soviet\save\<SaveName>\buildings.bin (read) & header.bin (write)
│   └── Triggers load on game load, save on Save / Autosave / Save As
├── hooks/game_hooks.{hpp,cpp}
│   └── Uses wrsr::game::GameContext (common/config/offsets.ini + AOB scanners) to gate Customs (building type 20) dispatches in EvaluateDistTask
```

---

## CustomsSpeedControl Architecture

```text
CustomsSpeedControl.dll
├── customs_speed/main.cpp
│   ├── Exports: RmlPluginQuery / RmlPluginLoad (RML v1) & TsmPlugin* (Tesmio API v4)
│   └── Registers the "Customs Speed" tab on the shared F9 WRSR Tweaks overlay
├── customs_speed/config.{hpp,cpp}
│   ├── Lock-free atomic Load/Unload multipliers (0.25x–100.0x) & Affect Rail toggle
│   └── INI persistence → <DllDir>\CustomsSpeedControl.ini
├── customs_speed/customs_hook.{hpp,cpp}
│   ├── Uses wrsr::game::GameContext + cargo_transfer_tick_aob + .pdata-scoped C3D_TIMER::PowerTime IAT filter
│   └── Scales pre-clamp transfer rates strictly at $TYPE_CUSTOMHOUSE (building type 20)
└── ../../src/overlay/mod_overlay.{h,cpp}
    └── Shared F9 multi-mod ImGui hub (hosts/connects to Local\WRSR_ModOverlay_<PID>)
```
