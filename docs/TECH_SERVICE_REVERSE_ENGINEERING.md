# Technical Services Reverse Engineering Notes (`SOVIET64.exe`)

## Target Binaries Verified

| Track | PE `TimeDateStamp` | `SizeOfImage` | `TechOffice_Tick` | `RoadNode_HasSnow` | `FindWaterTarget` | `FindSewageTarget` | `FindWasteTarget` |
|---|---|---|---|---|---|---|---|
| WRSR `1.2.0.9` (`latest`) | `0x6B05A574` | `0xAE9000` | `0x1DD2C0` | `0x5A3460` | `0x1956F0` | `0x195800` | `0x1B7410` |

All functions are resolved dynamically at runtime via `.text` AOB signatures in `mods/TechServiceControl/config/offsets.ini`.

---

## Technical Services Dispatch Flow

1. **Building Dispatch (`0x156C9A`)**:
   - Checks `BuildingDescriptor + 0x368 == 0x31` (`49`, `$TYPE_GARBAGE_OFFICE` / Technical Services Office) and calls `TechOffice_Tick(GameObject* gameObj, Building* techOffice)` (`0x1DD2C0`).
2. **Vehicle Collection & Target Rebuild (`TechOffice_Tick`)**:
   - Walks assigned vehicles at `techOffice + 0xC70` (`std::vector<Vehicle*>`).
   - Rebuilds reachable building targets at `techOffice + 0xF90` (`std::vector<Building*>`) periodically (`techOffice + 0xFC0` cooldown timer).
   - Calls the 4 service assigners in order:
     1. **Snow Assigner** (`0x1DDF70`)
     2. **Water Assigner** (`0x1DEC90`)
     3. **Sewage Assigner** (`0x1DEE80`)
     4. **Waste Assigner** (`0x1DF0B0`)

---

## Service Threshold Hooks

### 1. Snowplow Dispatch (`RoadNode_HasSnow`, `0x5A3460`)

- Signature: `bool __fastcall RoadNode_HasSnow(RoadSearchRequest* req, RoadNode* node)`
- Called from `RoadSearch::Run` (`0x5A8FE4`..`0x5A8FF0`) when `req->searchMode == 9` (`*(int32_t*)(req + 0xB8) == 9`).
- `SnowAssigner` (`0x1DE07E`) initializes `req->minSnowByte` (`*(int32_t*)(req + 0x20)`) to `0`.
- `RoadNode_HasSnow` loops over `node->segments` (`+0x28/+0x30`), finds `maxSnowByte` (`0..255`) across `segment->snowBytes` (`+0xE8/+0xF0`), and returns `true` if `req->minSnowByte < maxSnowByte` (`cmp esi, edx; jl`).
- **Override:** Temporarily set `*(int32_t*)(req + 0x20)` to `clamp(lround(snowplow_min_pct * 255.0f / 100.0f) - 1, 0, 254)` during the call when `searchMode == 9`, then restore the original value.

### 2. Water Delivery (`FindWaterTarget`, `0x1956F0`)

- Signature: `Building* __fastcall FindWaterTarget(GameObject* gameObj, Building* techOffice)`
- Iterates `techOffice + 0xF90/+0xF98` (`std::vector<Building*>`).
- Skips `BuildingDescriptor == nullptr`, `descType` in `{0x14 (20), 0x5C (92), 0x61 (97)}`, or piped water buildings (`*(uint8_t*)(bld + 0x133D) != 0` unless `descType == 0x5B (91)`).
- Calls `GetWaterFillRatio(gameObj, bld)` (`0x1953E0`), checks `0.0f <= ratio < 0.30f` (vanilla `0.30f`), checks `*(float*)(bld + 0x1310) <= 18.0f` and `!IsWaterTruckEnRoute(bld)` (`0x195180`), and selects the lowest-fill building.
- **Override:** Re-evaluates the exact loop using `water_below_pct / 100.0f` when enabled.

### 3. Sewage Pickup (`FindSewageTarget`, `0x195800`)

- Signature: `Building* __fastcall FindSewageTarget(GameObject* gameObj, Building* techOffice)`
- Iterates `techOffice + 0xF90/+0xF98` (`std::vector<Building*>`).
- Skips `BuildingDescriptor == nullptr`, `descType` in `{0x14 (20), 0x62 (98)}`, or piped sewage buildings (`*(uint8_t*)(bld + 0x133E) != 0` unless `descType == 0x5E (94)`).
- Calls `GetSewageFillRatio(gameObj, bld, 0)` (`0x195550`), checks `ratio > 0.70f` (vanilla `0.70f`), checks `!IsSewageTruckEnRoute(bld)` (`0x1952B0`), and selects the highest-fill building.
- **Override:** Re-evaluates the exact loop using `sewage_min_pct / 100.0f` when enabled.

### 4. Garbage / Waste Pickup (`FindWasteTarget` threshold block, `0x1B7599`)

- Inside `FindWasteTarget` (`0x1B7410`):
  - `0x1B7599`: `F3 44 0F 10 0D disp32` (`movss xmm9, [0.75f]`) — mixed waste / garbage stand / building bin fill ratio (`> 0.75f` vanilla).
  - `0x1B75C4` (`+43`): `F3 0F 10 2D disp32` (`movss xmm5, [0.60f]`) — specific waste fraction fill ratio (`> 0.60f` vanilla).
- **Override:** Allocates a `float[2]` page within ±2 GB of `SOVIET64.exe` and retargets both `disp32` operands to `g_wasteThresholds[0]` (mixed) and `g_wasteThresholds[1]` (fraction), writing `garbage_min_pct / 100.0f` when enabled or restoring `0.75f` / `0.60f` when disabled.

---

## Verified Struct Offsets

| Struct | Offset | Type | Field | Notes |
|---|---|---|---|---|
| `Building` (Tech Office) | `+0xF90/+0xF98` | `std::vector<Building*>` | `serviceTargets` | Reachable buildings in office range |
| `Building` (Target) | `+0x318` | `BuildingDescriptor*` | `descriptor` | `+0x368` (`+0x360` on `1.1.1.x`) is `BUILDINGTYPE_*` |
| `Building` (Target) | `+0x1310` | `float` | `waterPriorityMetric` | Checked `<= 18.0f` in `FindWaterTarget` |
| `Building` (Target) | `+0x133D` | `uint8_t` | `isPipedWater` | Non-zero if connected to water pipe |
| `Building` (Target) | `+0x133E` | `uint8_t` | `isPipedSewage` | Non-zero if connected to sewage pipe |
| `RoadSearchRequest` | `+0x20` | `int32_t` | `minSnowByte` | `0..255` threshold (`0` in vanilla `SnowAssigner`) |
| `RoadSearchRequest` | `+0xB8` | `int32_t` | `searchMode` | `9` = snowplow road search |
| `RoadNode` | `+0x28/+0x30` | `std::vector<RoadSegment*>` | `segments` | Connected road segments |
| `RoadSegment` | `+0xE8/+0xF0` | `std::vector<uint8_t>` | `snowBytes` | Per-point snow depth (`0..255`) |
