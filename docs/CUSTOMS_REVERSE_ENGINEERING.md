# Customs House Cargo Transfer Reverse Engineering Guide (`SOVIET64.exe`)

This guide explains how to capture the `SOVIET64.exe` **vehicle cargo-transfer tick** AOB signature for `CustomsSpeedControl` (`cargo_transfer_tick_aob`) using a Cheat Engine hardware write breakpoint on a parked truck's cargo float.

> **Important:** Always scan `.text` dynamically with wildcarded `disp32` / `rel32` displacements rather than hardcoding `.text` RVAs, because every WRSR hotfix shifts `.text` functions non-uniformly.

---

## 1. Known Engine Struct Offsets (Verified References)

These offsets are already configured in `common/config/offsets.ini` and `mods/CustomsSpeedControl/config/offsets.ini`:

| Struct | Offset | Type | Meaning |
|---|---|---|---|
| `Building` | `+0x318` | `BuildingDescriptor*` | Type descriptor pointer (`bld_desc_off`) |
| `BuildingDescriptor` | `+0x360` *(1.1.1.7–1.1.1.13)* / `+0x368` *(1.2.0.8)* | `int32_t` | `BUILDINGTYPE_*` (`20` = `$TYPE_CUSTOMHOUSE`) |
| `Vehicle` | `+0x398` | `Vehicle*` | Pulling locomotive (non-null for rail cars; target building lives at `pulling+0x528`) |
| `Vehicle` | `+0x3B8` / `+0x3C0` | `std::vector<CargoSlot>` | Cargo slots `{ Resource* res; int32_t pad; float amount; float quality; }` (16-byte stride) |
| `Vehicle` | `+0x528` / `+0x538` | `Building*` | Docked station/Customs building (`+0x538` override takes precedence when non-null for solo/road vehicles; `+0x4F0` is `nullptr` during road transfer) |
| `Vehicle` | `+0x594` | `int32_t` | Transfer state (`0` = idle/waiting, `1` = unloading, `2` = loading) |
| `Vehicle` | `+0x1708` *(1.1.1.7–1.1.1.13)* / `+0x1748` *(1.2.0.8–1.2.0.9)* | `VehicleType*` | Vehicle type descriptor |
| `VehicleType` | `+0x294` | `int32_t` | Category (`3` = rail wagon, `4` = locomotive, `5` = rail service) |

> **Note on Cargo Units:** WRSR stores cargo quantities internally in **game units**, whereas the UI displays `internal_amount * transport_class_factor` (the factor at `ResourceRecord + 0xCC + transport_class * 0x20`). For resources with factor `1.0` (e.g., gravel/coal on dumpers, or fuel/oil on tankers with factor `1.0`), the stored float matches the UI tonnage directly; for others (e.g. `0.35`), scan using **Unknown initial value → Increased value / Decreased value** while the truck transfers cargo.

---

## 2. Verified Live Findings (`SOVIET64.exe` v1.1.1.13, v1.2.0.8 & v1.2.0.9)

Captured via Cheat Engine hardware write breakpoint on an active truck's `CargoSlot.amount` (`+0x08`) inside a Customs House (`v1.1.1.13`, `PE TimeDateStamp 0x6A8D4087`):

- **Enclosing Function (`VehicleCargoTransferTick`):**
  - `.pdata` bounds on `v1.1.1.13`: RVA `0x68B3D0..0x698DE7` (`55,831` bytes).
  - `.pdata` bounds on `v1.2.0.8`: RVA `0x6BDA30..0x6CBEF7` (`58,567` bytes).
  - `.pdata` bounds on `v1.2.0.9`: RVA `0x6BDD00..0x6CC267` (`58,727` bytes).
  - **Calling convention (`__fastcall`):**
    ```cpp
    bool __fastcall VehicleCargoTransferTick(
        Vehicle* vehicle,        // RCX
        void*    outLoadDone,    // RDX
        void*    outUnloadDone,  // R8
        uint8_t  arg4,           // R9B
        uint8_t  arg5,           // [rsp+0x20]
        float    rateScale,      // [rsp+0x28]
        uint8_t  isLoadOnly,     // [rsp+0x30]
        uint8_t  isUnloadOnly    // [rsp+0x38]
    );
    ```
  - **Target building resolution (`veh` in `RCX`):**
    1. If `*(veh + 0x398)` (`pullingLocomotive`) != `nullptr`: target building is `*(pullingLocomotive + 0x528)`.
    2. Otherwise: target building is `*(veh + 0x538)` if non-null, falling back to `*(veh + 0x528)`.
    3. Verify `targetBld->desc` (`+0x318`) -> `desc->buildingType` (`+0x360` on 1.1.1.x, `+0x368` on 1.2.0.8) == `20` (`$TYPE_CUSTOMHOUSE`).
  - **Pre-clamp transfer rate mechanism (`C3D_TIMER::PowerTime`):**
    Instead of exposing a single top-level `dt` parameter, `VehicleCargoTransferTick` computes every cargo type's per-tick transfer delta internally by calling `C3DDLL64.dll!?PowerTime@C3D_TIMER@@QEAAMM_N0@Z` (`FF 15 <disp32>`) immediately **before** multiplying by `BuildingDescriptor + 0x824 / +0x828` (`$VEHICLE_LOADING_FACTOR` / `$VEHICLE_UNLOADING_FACTOR`) and **before** `minss` capacity clamping.
    - Non-cargo `PowerTime` calls inside the function update dwell/wait timers (`+0x598`, `+0x179C`/`+0x1834`), loading animation (`+0x1310`/`+0x1348`), electricity checks (`+0x10E0`), and worker-helper delta timers.
    - `customs_hook.cpp` combines a MinHook detour on `VehicleCargoTransferTick` (setting a thread-local context when inside a Customs House and allowed by `affect_rail`) with an IAT hook on `C3DDLL64.dll` `?PowerTime@C3D_TIMER@@QEAAMM_N0@Z` filtered by a pre-classified sorted list of cargo-transfer return addresses within `[funcBegin, funcEnd)` and gated by `*(int32_t*)(veh + 0x594)` (`1` = unload, `2` = load).
- **Verified Unique AOB (`cargo_transfer_tick_aob`):**
  ```ini
  48 8B C4 44 88 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? FE FF FF FF 0F 29 70 A8 0F 29 78 98
  ```

---

## 3. Re-Capturing `cargo_transfer_tick_aob` After Future Updates

### Step 1: Set Up an Isolated Test Case
1. Load a save (or empty map with free-build/cheat money) with **one Customs House** (`$TYPE_CUSTOMHOUSE`) and **one road depot**.
2. Buy a single road truck (e.g., an oil tanker loading `fuel` or a covered truck loading `clothes`).
3. Give it a simple line: **Load at Customs House** (set to wait until loaded) and pause the game right as the truck enters the Customs House bay.

### Step 2: Locate the Truck's Live Cargo Float
1. Attach **Cheat Engine** to `SOVIET64.exe`.
2. Set **Value Type** to `Float`.
3. Unpause briefly (or step at `1x` speed) while scanning:
   - Start with **Unknown initial value** (or **Value between** if you know the resource's transport factor ≈ 1.0).
   - Let the truck load a few ticks, pause, and scan **Increased value**.
   - Repeat 3–5 times until you narrow down to a handful of floats that climb monotonically while loading and reset to `0.0` when emptied.
   - Tip: Verify the right address by checking the pointer `addr - 0x08`: in WRSR's `CargoSlot`, `+0x00` is a valid 64-bit `Resource*` pointer (into `ResourceVector`) and `+0x08` is the `float` cargo amount.

### Step 3: Set a Hardware Write Breakpoint
1. Right-click the verified cargo float address → **Find out what writes to this address** (use **VEH Debugger** or **DBVM** in Cheat Engine settings so `SOVIET64.exe` runs smoothly).
2. Unpause the game for 1–2 seconds while the truck loads at the Customs House, then pause again.
3. Cheat Engine will record the instruction writing the updated cargo float (typically `movss [rcx+08], xmm0` or `addss` followed by `movss` inside a capacity-clamped loop).
4. Also repeat the test while **unloading** an export truck at the Customs House to confirm whether Load and Unload share the same tick function or dispatch to two sibling functions/branches.

### Step 4: Inspect Function Bounds & Calling Convention
1. Click **Show disassembler** on the breakpoint hit in `SOVIET64.exe`.
2. Scroll up to the function prologue (or check `.pdata` `RUNTIME_FUNCTION` bounds in x64dbg / Ghidra / Binary Ninja).
3. Record:
   - Exact **register arguments** (`RCX`, `RDX`, `R8`, `R9`, `XMM0..XMM3`): which parameter is `Vehicle*`, which is `Building*` or `Storage*`, which holds the per-tick transfer rate / delta-time (`dt`), and which flag/branch selects **Load** vs **Unload**.
   - Where **capacity clamping** occurs (`minss` / `comiss` against remaining vehicle capacity or building storage limit) so our detour scales the rate **before** capacity clamping.
4. Update the prototype and argument mapping in `mods/CustomsSpeedControl/src/customs_speed/customs_hook.cpp` (`PFN_CargoTransferTick` / `detourCargoTransferTick`) to match the verified signature.

### Step 5: Build & Validate the Unique AOB
1. Copy the first ~24–36 bytes of the function prologue (or a distinctive instruction cluster inside the function).
2. Replace all `disp32` RIP-relative offsets and `rel32` `call`/`jmp` targets with `??`. Keep all opcodes, ModRM/SIB bytes, and struct offsets (`+0x4F0`, `+0x3B8`, etc.) intact.
3. Verify the AOB returns **exactly 1 hit** in `SOVIET64.exe`'s `.text` section (Cheat Engine → **Memory Scan** → **Array of byte**, writable=unchecked, executable=checked).

### Step 6: Live-Test Without Recompiling
1. Paste your candidate AOB into `CustomsSpeedControl.ini` beside `CustomsSpeedControl.dll`:
   ```ini
   [signatures]
   cargo_transfer_tick_aob=48 89 5C 24 08 ...
   ```
2. Launch WRSR, press **`F9`**, and check the **Customs Speed** tab status line (`Hook: ACTIVE @ SOVIET64.exe+0x...`).
3. Once confirmed across builds (e.g. `1.1.1.7` and `1.2.0.8`), commit the AOB into `mods/CustomsSpeedControl/config/offsets.ini`.
