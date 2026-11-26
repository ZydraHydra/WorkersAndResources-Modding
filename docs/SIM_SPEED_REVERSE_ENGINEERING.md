# AdaptiveSimSpeed Reverse Engineering (`SOVIET64.exe`)

Verified live against `SOVIET64.exe` (Base `0x7FF638EC0000`, `SizeOfImage = 0xAE9000` / `11,440,128` bytes, WRSR `1.2.0.9`) via Cheat Engine MCP Bridge.

---

## 1. Main Frame Loop & Step Architecture (`0x7FF638FDD712`–`0x7FF638FDD9E1`)

In `SOVIET64.exe`, the main frame loop (`0x7FF638FDD8C9`–`0x7FF638FDDEB1`) executes in the following order:

1. **`0x7FF638FDD712`–`0x7FF638FDD720` (`C3D_TIMER::Start`):**
   - Updates `g_timer` (`0x7FF6398DEEF0`, `C3D_TIMER` struct) and computes raw frame FPS into `*(float*)(g_timer + 0x00)` and `*(float*)(g_timer + 0x04)`.
2. **`0x7FF638FDD7B2`–`0x7FF638FDD861` (Game Speed Scaling):**
   - Reads `GameSpeed` (`int32_t` at `0x7FF6398DF4EC`, i.e., `GameObject + 0x5CC`):
     - `0` = **Paused**: sets `*(uint8_t*)(g_timer + 0x0C) = 1` (`paused`).
     - `1` = **Normal (`1x`)**: leaves `*(float*)(g_timer + 0x00)` unscaled.
     - `2` = **Fast Forward (`>>`)**: when `*(int32_t*)(GameObject + 0x5D4) == 0`, multiplies `*(float*)(g_timer + 0x00)` by `0.35f` (`0x7FF6398112F4`). Because `C3DDLL64.dll!?PowerTime@C3D_TIMER@@QEAAMM_N0@Z` divides by `*(float*)(g_timer + 0x00)` when `ignorePause == false` (`r9b == 0`), this scales every simulation `dt` by `1.0f / 0.35f ≈ 2.857x`.
     - Meanwhile, `*(float*)(g_timer + 0x04)` remains **unscaled** (used by `C3D_TIMER::PowerTime(..., ignorePause=true)` for camera panning, UI, and input).
3. **`0x7FF638FDD883` / `0x7FF638FDD8DF`:** Input polling, camera/audio listener update (`0x7FF6392B7670`).
4. **`0x7FF638FDD9C9`:** Scene draw (`0x7FF6391CDD10`).
5. **`0x7FF638FDD9D5`:** `lea rcx, [0x7FF6398DEF20]; call 0x7FF6391EB8C0` (**`SimStep(GameObject*)`**).

### Step Architecture Conclusion
`SOVIET64.exe` calls `SimStep(GameObject*)` **once per frame** and achieves Fast Forward (`>>`) by scaling `C3D_TIMER + 0x00` (`sim_fps *= 0.35f`), which multiplies `dt` by `~2.857x`.

To implement **hardware-adaptive fixed-step sub-stepping** without double-multiplying or passing oversized `dt` values:
- Inside `detourSimStep(void* gameObject)`, when boosting is active, we temporarily set `*(float*)(g_timer + 0x00) = *(float*)(g_timer + 0x04)` (restoring `1.0x` `dt` per sub-step) via an RAII guard.
- We then invoke `origSimStep(gameObject)` for the baseline `1x` tick, measure `single_tick_ms` with `QueryPerformanceCounter`, and execute additional `1x` sub-steps while `sub_steps < max_speed_multiplier` and `elapsed_ms + (single_tick_ms * 1.25f) <= budget_ms`.

---

## 2. Disassembly Reference

```asm
; Main loop speed check (RVA 0x11D7B2, 1 unique match for game_speed_ref_aob)
7FF638FDD7B2 - 8B 05 341D9000        - mov eax,[7FF6398DF4EC]       ; GameSpeed (GameObject + 0x5CC)
7FF638FDD7B8 - 85 C0                 - test eax,eax
7FF638FDD7BA - 75 2C                 - jne 7FF638FDD7E8
7FF638FDD7BC - 40 88 2D 39179000     - mov [7FF6398DEEFC],bpl       ; C3D_TIMER + 0x0C (paused)
7FF638FDD7C3 - E9 A7000000           - jmp 7FF638FDD86F
...
7FF638FDD7E8 - 83 F8 02              - cmp eax,02                   ; Fast Forward (>>)
7FF638FDD7EB - 75 76                 - jne 7FF638FDD863
...
7FF638FDD849 - F3 0F10 05 9F169000   - movss xmm0,[7FF6398DEEF0]    ; C3D_TIMER + 0x00 (sim_fps)
7FF638FDD851 - F3 0F59 05 9B3A8300   - mulss xmm0,[7FF6398112F4]    ; * 0.35f (~2.857x dt)
7FF638FDD859 - F3 0F11 05 8F169000   - movss [7FF6398DEEF0],xmm0

; SimStep call site (RVA 0x11D9CE, 1 unique match for sim_step_call_aob)
7FF638FDD9CE - 48 8D 0D 4B159000     - lea rcx,[7FF6398DEF20]       ; GameObject*
7FF638FDD9D5 - E8 E6DE2000           - call 7FF6391EB8C0            ; SimStep(GameObject*)
7FF638FDD9DA - 48 8D 0D 3F159000     - lea rcx,[7FF6398DEF20]
7FF638FDD9E1 - E8 CAF95200           - call 7FF63950D3B0
```

---

## 3. Verified Unique AOB Signatures

- **`game_speed_ref_aob`** (1 hit at RVA `0x11D7B2`):
  `8B 05 ?? ?? ?? ?? 85 C0 75 2C 40 88 2D ?? ?? ?? ?? E9 ?? ?? ?? ?? 44 38 35`
  - `+2` (next IP `+6`): RIP-relative `disp32` -> `int32_t* GameSpeed` (`0x7FF6398DF4EC`)
  - `+13` (next IP `+17`): RIP-relative `disp32` -> `uint8_t* C3D_TIMER::paused` (`+0x0C`), so `C3D_TIMER* = ptr - 0x0C` (`0x7FF6398DEEF0`)
- **`sim_step_call_aob`** (1 hit at RVA `0x11D9CE`):
  `48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 38 35 ?? ?? ?? ?? 74 0C 48 8D 0D ?? ?? ?? ?? E8`
  - `+8` (next IP `+12`): `E8 <disp32>` -> `SimStep(void* gameObject)` (`0x7FF6391EB8C0`, size `0xAA76`)
