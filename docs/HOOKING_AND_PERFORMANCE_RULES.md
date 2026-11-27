# Hooking and Performance Rules (`SOVIET64.exe` / `C3DDLL64.dll`)

These rules apply to **both** `GloriousRepublicModLoader` (`GRML`) and all native plugins in `WorkersAndResources-Modding`.

---

## 1. Hot-Path Performance Rules (Zero-Alloc & Lock-Free)

Simulation, rendering, and OS file hooks (`CreateFileW`, `GetFileAttributesW`, `EvaluateDistTask`, `C3D_TIMER::PowerTime`, `IDXGISwapChain::Present`) execute thousands of times per second.

1. **Zero Heap Allocations on Hot Paths:**
   - Never construct `std::string`, `std::wstring`, `std::vector`, or call `malloc`/`new` inside a hot hook.
   - Use fixed stack buffers (`char[256]`, `wchar_t[512]`), `std::string_view` / `std::wstring_view`, and pre-built immutable lookup maps.
2. **Lock-Free Reads on Hot Paths:**
   - Never acquire a `std::mutex` on the simulation or file-I/O hot path.
   - Publish config/state changes from the UI/config thread into `std::atomic<T>` POD fields or fixed-size atomic slot arrays (e.g., `FastThresholdSlot[256]`), reading with `std::memory_order_relaxed` or `acquire`.
3. **Single-Pass Startup Scanning:**
   - Never rescan `.text` repeatedly for each plugin or feature. Register AOBs once and scan `.text` in a single pass at bootstrap via `GrmlHostApi::RegisterPatternScan`.
4. **Instrument Hot Hooks:**
   - Wrap hook bodies with `WRSR_PROFILE_HOOK("ModName::HookName")` (or report via `GrmlUiApi::ReportHookTiming`) so frame cost and call counts are visible in the `F9` Mod Settings telemetry table.

---

## 2. Hook Safety & Selection Hierarchy

Prefer the least invasive hook mechanism that satisfies the requirement:

1. **Import Address Table (IAT) Swap** (`SOVIET64.exe` or `C3DDLL64.dll`) — safest; zero instruction bytes overwritten.
2. **Vtable Slot Swap** (e.g., DX11 `IDXGISwapChain::Present` / `ResizeBuffers`, `IDirectInputDevice8W::GetDeviceState`) — clean chaining with MinHook or direct slot swap.
3. **Data Pointer / `disp32` Operand Repoint** — allocate within `+-2 GiB` (`VirtualAlloc` near RIP) and update only the 32-bit displacement after verifying the instruction bytes. Never overwrite shared `.rdata` float constants (MSVC pools literals).
4. **Call-Site `rel32` Patch** — preferred over prologue hooks when a function has a `/GS` 13-byte prologue.
5. **Inline Detour (Last Resort)** — requires at least **14 stolen bytes** (`FF 25 00000000` + 64-bit address) ending cleanly on an instruction boundary, with zero stolen RIP-relative instructions.

---

## 3. AOB Signatures, SEH & Multi-Version Resilience

1. **Never Hardcode `.text` RVAs:**
   - Always resolve game targets via wildcarded `.text` AOB signatures (`??` on relative displacements, offsets, and stack frame sizes) externalized in `grml_offsets.ini` / `offsets.ini`.
2. **Verify Exact Single Match (`count == 1`):**
   - Check that an AOB matches **exactly once** across `.text` (unless explicitly scanning all call sites). If an optional signature misses (`count != 1`), log the exact pattern name and disable only that feature—never crash `SOVIET64.exe`.
3. **Guard Foreign Memory Reads with SEH:**
   - Wrap dereferences of raw engine structs (`ResourceVector`, `DistTask`, `Building`, `C3D` objects) in `__try` / `__except (EXCEPTION_EXECUTE_HANDLER)` inside helper functions free of C++ objects with destructors.
4. **Never Share C++ Objects or Heap Across DLL Boundaries:**
   - Every DLL builds with static CRT (`/MT`). All cross-DLL interfaces (`grml_api.h`, `loader_abi.hpp`) must remain **pure C** (`extern "C"`, POD types, `abi_version` + `struct_size`), with each DLL freeing only its own allocations.
5. **UTF-8 Without BOM & Wide Win32 Paths:**
   - All `.ini` and `.json` files must be UTF-8 **without BOM**.
   - All Windows filesystem paths (Steam Workshop `784150`, saves, local plugins) must use `wchar_t` / `std::filesystem::path` and Win32 `*W` APIs for non-ASCII path safety.
