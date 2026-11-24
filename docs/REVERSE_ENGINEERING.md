# Reverse Engineering Notes (`SOVIET64.exe`)

## Target Binary Overview

| Property | Value |
|---|---|
| Executable | `SOVIET64.exe` (PE32+ AMD64 / x86-64, ASLR enabled, no CFG, no packer) |
| Engine DLL | `C3DDLL64.dll` (2,218 exports with intact MSVC mangling) |
| Local Observed Build | Size: `10,615,808` bytes · PE TimeDateStamp: `0x6B0097B2` · SHA-256: `91A2FF66FD02E02A51F38F4D2106CCB56AF9510AF9A6FB7E7B93AE4249358EA7` |

> **Important:** Community references (`TesmioLoader` / `wrsr-agents`) document builds `v1.1.1.7` (`0x69C4098C`), `v1.1.1.9` (`0x6A3EB6AD`, `10,308,608` bytes), and `v1.1.1.13` (`0x6A8D4087`). The current Steam binary (`0x6B0097B2`, `10,615,808` bytes) is newer — **never hardcode `.text` RVAs**. Always resolve targets via `.text` AOB signatures filtered against `.pdata` function bounds, and re-verify every byte offset against live memory.

---

## Loader Export Contracts

### Republic Mod Loader (RML API v1)

- Exports queried:
  - `int __cdecl RmlPluginQuery(uint32_t host_api_version, RmlPluginDescriptor* out_descriptor)`
  - `int __cdecl RmlPluginLoad(const RmlHostApi* host)`
- Sidecar manifest (`<ModName>.rml.json`) schema:
  - `schemaVersion`: `1`
  - `id`: lowercase `[a-z0-9._-]` (must match `out_descriptor->id` in `RmlPluginQuery`)
  - `name`, `version`, `author`, `description`, `dll` (`"<ModName>.dll"`)
  - `loaderApi`: `{ "min": 1, "max": 1 }`
  - `defaultPriority`, `enabledByDefault`
- Discovery path in Workshop items: `<WorkshopItem>\plugin\*.dll` or `<WorkshopItem>\plugins\*.dll`.

### TesmioLoader (API v4 Compatibility)

- Exports queried:
  - `unsigned TsmPluginApiVersion(void)` (returns `4`)
  - `int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)` (`0` = success)
  - `int TsmPluginStart(void)` (optional phase 2, `0` = success)

---

## Resource Price Table Internals (`ResourceRecord`)

Struct offsets come from community RE (`TesmioLoader` `02-findings.md`, corrected in `wrsr-agents` CC BY 4.0 from `WRSR_Mail` on `v1.1.1.9`) and are marked `[community-v1.1.1.9]` until re-verified on `0x6B0097B2`:

- **Resource Vector:** `std::vector<ResourceRecord>` inside the global `GameObject` (`game + 0xC2B0`), stride **`0x340` (832 bytes)**, 57 base-game resources. A map load clears (`end = begin`) without freeing, so cached pointers must be re-validated after loads.
- **Market Snapshots on `GameObject`** (`[community-v1.1.1.9]`):
  - `+0x114C0`: buy USD snapshot
  - `+0x114D8`: buy RUB snapshot
  - `+0x114F0`: sell USD snapshot
  - `+0x11508`: sell RUB snapshot
- **`ResourceRecord` (`0x340` bytes) Price Fields** (`[community-v1.1.1.9]`):

| Offset | Type | Field | Notes |
|---|---|---|---|
| `+0x00` | `char[0x40]` | `name` | Inline NUL-terminated ASCII resource name (bytes after NUL contain stack tail) |
| `+0x44` | `int32_t` | `priceKind` | `-1` workers, `-2` eletric, `-3` vehicles, `-4` trains, `-5` heat, `0..4` tradeable |
| `+0x58` | `float` | `buyPriceUsd` | Live buy price (USD) |
| `+0x5C` | `float` | `buyPriceRub` | Live buy price (RUB) |
| `+0x60` | `float` | `sellPriceUsd` | Live sell price (USD) — customs pays as-is |
| `+0x64` | `float` | `sellPriceRub` | Live sell price (RUB) — customs pays as-is |
| `+0x78` | `float` | `basePriceRub` | `$Economy_BaseRUB` (solver input; drifts by random walk) |
| `+0x7C` | `float` | `basePriceUsd` | `$Economy_BaseUSD` |
| `+0x88` | `float` | `sellMult0` | Sell multiplier (`~0.95f`) |
| `+0x8C` | `float` | `buyMult0` | Buy multiplier (`~1.05f`) |
| `+0xA8` | `float` | `sellMult1` | Market block 2 sell multiplier (`~0.95f`) |
| `+0xAC` | `float` | `buyMult1` | Market block 2 buy multiplier (`~1.05f`) |

---

## Save Header (`header.bin`) & `stats.ini` Cross-Check

From `burnshall-ui/SovietRepublic-MCP` (MIT):
- `header.bin` stores live money/date updated every autosave: `0x184` (`float` USD), `0x188` (`float` RUB), `0x198` (`uint32` day), `0x19C` (`uint32` year).
- `stats.ini` logs periodic `$Economy_PurchaseCostRUB` / `$Economy_PurchaseCostUSD` and `$Resources_Import*` / `$Resources_Export*` sections — ideal for cross-checking `TradePriceLimits`'s live price reads and verifying blocked trades in `SovietRepublic-MCP`.

---

## Signatures (`TradePriceLimits` — Verified on `v1.1.1.7` & Steam `0x6B0097B2`)

Both signatures in `mods/TradePriceLimits/src/hooks/game_hooks.hpp` resolve dynamically in `.text` with **1 unique match** on both `v1.1.1.7` (`0x69C4098C`) and the newer Steam binary (`0x6B0097B2`):

### Target 1: `SIG_PRICE_TABLE` (`ResourceVector` + Dynamic Stride)

- Matches `0xD3FB4` (`v1.1.1.7` -> `ResourceVector` @ `0x9E11C0`, stride `0x340`) and `0xEBB94` (`0x6B0097B2` -> `ResourceVector` @ `0xA2B268`, stride `0x380`).
- Reads `disp32` at `+14` (`LEA RCX, [ResourceVector]`) and `imm32` stride at `+45` (`IMUL RAX, R8, stride`).

### Target 2: `SIG_DISPATCH_CHECK` (`EvaluateDistTask` + Customs Currency Flag)

- Matches `EvaluateDistTask(GameObject*, Task0x80*)` at `0x1E3B60` (`v1.1.1.7`) and `0x1FE850` (`0x6B0097B2`), called by both `RoadDistOffice` (`0x1C5FE0`) and `RailDistOffice` (`0x1C6D60`).
- `Task0x80` fields (`[code]`):
  - `+0x00`: `ResourceRecordHeader*`
  - `+0x10`: source `Building*` (`BUILDINGTYPE_CUSTOMHOUSE == 20` -> **Buy / Import**)
  - `+0x48`: destination `Building*` (`BUILDINGTYPE_CUSTOMHOUSE == 20` -> **Sell / Export**)
  - `+0x28` / `+0x60`: `double` dispatch scores (zeroing these + `+0x40/+0x42/+0x78/+0x7A` suppresses dispatch).
- **Customs House Currency (`[code]` from `0x173991`):**
  - `*(int32_t*)(building + 0x9EC) == 3` -> Western / NATO border (**USD**, uses `buyPriceUsd` / `sellPriceUsd`).
  - `*(int32_t*)(building + 0x9EC) == 4` -> Soviet border (**RUB**, uses `buyPriceRub` / `sellPriceRub`).
