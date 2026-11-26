# Construction Office Reverse Engineering Notes (`SOVIET64.exe`)

## Target Binaries Verified

| Track | PE `TimeDateStamp` | `SizeOfImage` | `CO_Tick` | `DispatchCOTask` | `PickBestCOVehicle` | `EvalCOTaskFill` |
|---|---|---|---|---|---|---|
| WRSR `1.2.0.9` (`latest`) | `0x6B05A574` | `0xAE9000` | `0x15F580` | `0x169440` | `0x16E490` | `0x1674D0` |
| WRSR `1.2.0.8` | `0x6B0097B2` | `0xAE9000` | — | `0x1695B0` | `0x16E600` | `0x167640` |

All functions are resolved dynamically at runtime via `.text` AOB signatures in `mods/CODispatchControl/config/offsets.ini`.

---

## Construction Office Dispatch Flow

1. **Building Dispatch (`0x156A6A` / `0x156A9A`)**:
   - Checks `BuildingDescriptor + 0x368 == 12` (`BUILDINGTYPE_CONSTRUCTION_OFFICE`) or `28` (`BUILDINGTYPE_CONSTRUCTION_OFFICE_RAIL`) and calls `CO_Tick(GameObject* gameObj, Building* coBuilding)` (`0x15F580`).
2. **Available Vehicle Collection (`CO_Tick + 0xDE`, `0x15F65E`)**:
   - Iterates `coBuilding + 0xC70` (`std::vector<Vehicle*>`, all vehicles assigned to the Construction Office).
   - A vehicle is parked/idle in the CO when `*(Building**)(veh + 0x4F8) == coBuilding` and `*(Building**)(veh + 0x4F0) == coBuilding` (or `veh + 0x528 == coBuilding`).
3. **Vehicle Selection (`PickBestCOVehicle`, `0x16E490`)**:
   - Signature: `Vehicle* __fastcall PickBestCOVehicle(GameObject* gameObj, float neededAmount, ResourceRecord* res, std::vector<Vehicle*>* availableVehicles)`
   - Compares `res` against `*(ResourceRecord**)(gameObj + 0xC368)` (the global Workers `ResourceRecord*`):
     - **Workers**: reads passenger capacity from `VehicleType + 0x8654` (`float`) when `*(int32_t*)(VehicleType + 0x8650) == 7`.
     - **Materials**: calls `GetVehicleResourceCapacity(veh, res, 1, hasSkill)` (`0x6F1E00`).
4. **Vehicle-Task Dispatch Gate (`DispatchCOTask`, `0x169440`)**:
   - Signature: `bool __fastcall DispatchCOTask(GameObject* gameObj, COTask64* task, Building* coBuilding)`
   - Called from `CO_Tick` (`0x16138F`, `0x161B9B`) and Helicopter CO (`0x1F3BB3`).
   - Returning `false` (`al == 0`) causes `CO_Tick` (`0x161BA2: je 0x161E3D`) to skip dispatching that `Vehicle*` for the task without altering any save state.
   - On successful material dispatch (`0x169A3A`), sets `*(int32_t*)(veh + 0x50C) = 8` and starts the trip via `0x41F0F0`.

---

## Verified Struct Offsets

### `COTask64` (`0x40` bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| `+0x00` | `Building*` | `siteBuilding` | Target construction site (`+0x610` = construction phase array, stride `0x21B8`) |
| `+0x08` | `Building*` | `srcBuilding` | Assigned source building (e.g., gravel/asphalt plant or bus stop) |
| `+0x10` | `Vehicle*` | `vehicle` | Candidate vehicle (truck, bus, or self-propelled mechanism) |
| `+0x18` | `Vehicle*` | `carrierVehicle` | Flatbed/carrier `Vehicle*` when transporting a mechanism, else `nullptr` |
| `+0x20` | `ResourceRecord*` | `resource` | `nullptr` for mechanism/equipment transport; `*(gameObj + 0xC368)` (`priceKind == -1`) for workers; material `ResourceRecord*` otherwise |
| `+0x28` | `int32_t` | `taskFlags` | Task mode flags |
| `+0x2C` | `float` | `score` | Dispatch priority score |
| `+0x30` | `float` | `fillRatio` | `min(1.0f, remainingNeeded / vehCap)` |
| `+0x34` | `float` | `remainingNeeded` | Remaining tons of material or remaining workdays for the current construction phase |

### `Vehicle` & `VehicleType`

| Struct | Offset | Type | Field | Notes |
|---|---|---|---|---|
| `Vehicle` | `+0x4F0` | `Building*` | `currBuilding` | Equals `coBuilding` when parked in the CO |
| `Vehicle` | `+0x4F8` | `Building*` | `homeBuilding` | Owning Construction Office `Building*` |
| `Vehicle` | `+0x50C` | `int32_t` | `dispatchState` | Set to `8` when dispatched |
| `Vehicle` | `+0x1748` (`0x1708` on `1.1.1.x`) | `VehicleType*` | `vehicleType` | Vehicle type descriptor |
| `VehicleType` | `+0x294` | `int32_t` | `category` | Rail/road category |
| `VehicleType` | `+0x8650` | `int32_t` | `transportClass` | `7` for passenger/bus capacity |
| `VehicleType` | `+0x8654` | `float` | `passengerCapacity` | Seat count for buses/microbuses |
