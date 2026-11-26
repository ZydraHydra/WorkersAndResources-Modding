# Multi-Bay Road Building Apron Reverse Engineering Notes (`SOVIET64.exe`)

## Target Binary Verified

| Track | PE `TimeDateStamp` | `SizeOfImage` | `RoadVehicleTick` | Apron Entry Gate | Apron Exit Gate |
|---|---|---|---|---|---|
| WRSR `1.2.0.9` (`latest`) | `0x6B05A574` | `0xAE9000` | `0x6CCDC0` | `0x6CD7B8` | `0x6CF1B9` |

All addresses are resolved dynamically at runtime via `.text` AOB signatures in `mods/SmartBayAccess/config/offsets.ini`.

---

## Road Vehicle Building Entry / Exit State Machine (`RoadVehicleTick`)

`RoadVehicleTick(Vehicle* veh, ...)` (`0x6CCDC0`, size `0xA24E` bytes, single-threaded main simulation tick) manages road vehicle movement into and out of building parking bays.

### 1. Apron Movement Gate Check — Entry (`0x6CD7B8`)
```asm
; r15 = Vehicle*, r13 = 0
mov rdx, [r15+0x528]             ; rdx = Vehicle->targetBuilding
cmp [rdx+0xBB0], r13             ; Building->activeApronMover == nullptr ?
jne wait_outside                 ; If non-null, vehicle waits outside gate
cmp dword ptr [r15+0x510], -1    ; Vehicle->enteringBayId == -1 ?
jne wait_outside
; Per-bay slot reservation loop (ecx = 0..31):
cmp [rdx+r8+0xB90], r13b         ; Building->bayOccupied[ecx] == 0 ?
mov rax, [rdx+0xB80]             ; bays.end
sub rax, [rdx+0xB78]             ; bays.begin
sar rax, 5                       ; bay_count = (end - begin) / 0x20
cmp rax, r8                      ; ecx < bay_count ?
ja  reserve_bay
```
When a free bay `ecx` is found:
- Sets `Building->bayOccupied[ecx] = 1` (`+0xB90 + ecx`)
- Sets `Vehicle->enteringBayId = ecx` (`+0x510`)
- If `Building->descriptor->noApronLock == 0` (`BuildingDescriptor + 0xB89`):
  - Sets `Building->activeApronMover = veh` (`+0xBB0`)
  - Sets `Vehicle->apronLockedBuilding = bld` (`+0xC80`)

### 2. Apron Movement Gate Check — Exit (`0x6CF1B9`)
```asm
mov eax, [r15+0x56C]             ; Vehicle->parkedBayId
test eax, eax
jns check_exit_lock
cmp eax, -10
jg  skip_exit_lock
check_exit_lock:
cmp qword ptr [rcx+0x5D0], 0
je  skip_exit_lock
cmp qword ptr [rcx+0xBB0], 0     ; Building->activeApronMover == nullptr ?
jne wait_in_bay
; Acquire exit apron lock:
mov rax, [r15+0x390]             ; veh (self-pointer)
mov [rcx+0xBB0], rax             ; Building->activeApronMover = veh
mov [r15+0xC80], rax             ; Vehicle->apronLockedBuilding = bld
```

### 3. Active-Mover Lock Release
When a vehicle completes driving into its bay or finishes backing out onto the road (`0x6CD877`, `0x6CDC30`, `0x6CE358`, `0x6CEED4`, `0x6CF18D`, `0x6CFD40`, `0x6D1562`):
```asm
mov rax, [r15+0xC80]             ; Vehicle->apronLockedBuilding
cmp [rax+0xBB0], r15             ; Building->activeApronMover == veh ?
jne skip_clear
mov [rax+0xBB0], 0               ; Clear Building->activeApronMover = nullptr
skip_clear:
mov [r15+0xC80], 0               ; Clear Vehicle->apronLockedBuilding = nullptr
```

---

## Verified Struct Offsets

| Struct | Offset | Type | Field | Notes |
|---|---|---|---|---|
| `Vehicle` | `+0x390` | `Vehicle*` | `selfPtr` | Verified equal to `this` (`veh + 0x390 == veh`) |
| `Vehicle` | `+0x398` | `Vehicle*` | `pullingLocomotive` | Non-null for rail cars |
| `Vehicle` | `+0x4F0` | `Building*` | `currBuilding` | Current building when inside/parked |
| `Vehicle` | `+0x510` | `int32_t` | `enteringBayId` | `-1` when not entering |
| `Vehicle` | `+0x528` | `Building*` | `targetBuilding` | Target / docked building |
| `Vehicle` | `+0x56C` | `int32_t` | `parkedBayId` | `>= 0` (or `<= -10`) when parked/exiting, `-1` otherwise |
| `Vehicle` | `+0xC80` | `Building*` | `apronLockedBuilding` | Building whose `+0xBB0` apron lock this vehicle holds |
| `Vehicle` | `+0x1748` (`0x1708` on `1.1.1.x`) | `VehicleType*` | `vehicleType` | `+0x294` is category (`3..5` = rail) |
| `Building` | `+0x318` | `BuildingDescriptor*` | `descriptor` | `+0xB89` (`uint8_t`) = `noApronLock` |
| `Building` | `+0xA40 / +0xA48` | `std::vector<Bay>` | `parkingBays` | Customs/parking bay vector (stride `0x20`; `+0xA58` occupied, `+0xA78` `Vehicle*[32]`) |
| `Building` | `+0xB78 / +0xB80` | `std::vector<Bay>` | `bays` | Station/depot bay vector (stride `0x20`; `bay_count = (end - begin) >> 5`) |
| `Building` | `+0xB90` | `uint8_t[32]` | `bayOccupied` | Per-bay reservation flags |
| `Building` | `+0xBB0` | `Vehicle*` | `activeApronMover` | Vanilla single-vehicle apron movement lock |

---

## Verified Unique AOB Signatures (`SOVIET64.exe`)

- **`road_vehicle_tick_aob`** (1 hit at `0x6CCDC0`):
  ```ini
  48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 85 ?? ?? ?? ?? FE FF FF FF
  ```
- **`apron_entry_gate_aob`** (1 hit at `0x6CD7B8`):
  ```ini
  49 8B 97 28 05 00 00 4C 39 AA B0 0B 00 00 0F 85 ?? ?? ?? ?? 41 83 BF 10 05 00 00 FF 0F 85 ?? ?? ?? ??
  ```
- **`apron_exit_gate_aob`** (1 hit at `0x6CF1B9`):
  ```ini
  41 8B 87 6C 05 00 00 85 C0 79 ?? 83 F8 F6 7F ?? 48 83 B9 D0 05 00 00 00 74 ?? 48 83 B9 B0 0B 00 00 00 75 ??
  ```
