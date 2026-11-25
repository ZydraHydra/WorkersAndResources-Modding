# Workspace Architecture

## Multi-Mod Monorepo Design

This repository hosts multiple independent native x64 C++20 plugins for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`), sharing a common CMake build graph and shared utility library (`common/`).

```text
WorkersAndResources-Modding/
├── CMakeLists.txt            # Root FetchContent (MinHook, ImGui docking, mINI) + targets
├── common/                   # Shared static library (wrsr_common)
│   └── include/wrsr/
│       ├── loader_abi.hpp    # Clean-room RML API v1 & Tesmio API v4 C ABI
│       ├── logger.hpp        # Thread-safe log writer next to SOVIET64.exe
│       └── pe_utils.hpp      # PE section lookup, AOB scanner, IAT patcher
└── mods/
    └── TradePriceLimits/     # Individual plugin DLL + manifest + Workshop VDF
```

### Why a Monorepo?

1. **Single Dependency Cache:** MinHook, Dear ImGui (`docking` branch), and mINI are fetched once via `FetchContent` into `build/` and shared across every mod DLL.
2. **Static CRT (`/MT`):** Every target builds with `/MT` so Steam Workshop subscribers never hit missing Visual C++ Redistributable DLL errors.
3. **Clean-Room Loader Compatibility:** `wrsr/loader_abi.hpp` defines only the binary C ABI contracts (`RmlPluginQuery`/`RmlPluginLoad` and `TsmPluginApiVersion`/`TsmPluginInit`/`TsmPluginStart`) without bundling third-party GPL source files.

---

## TradePriceLimits Architecture

```text
TradePriceLimits.dll
├── main.cpp
│   ├── Exports: RmlPluginQuery / RmlPluginLoad (RML v1 — tested vs RML v1.0.1)
│   ├── Exports: TsmPluginApiVersion / TsmPluginInit / TsmPluginStart / TsmPluginTargetVersion (Tesmio API v4, 0.4.5)
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
└── hooks/game_hooks.{hpp,cpp}
    ├── Text-section AOB scanner via wrsr::pe::aobScanStr (cross-version signature resolution)
    ├── SIG_PRICE_TABLE: ResourceVector + dynamic record stride (0x340 on v1.1.1.7, 0x380 on v1.2.0.8)
    └── SIG_DISPATCH_CHECK: EvaluateDistTask hook gating Customs (building type 20) dispatches + 60s sim-linked ring buffer
```
