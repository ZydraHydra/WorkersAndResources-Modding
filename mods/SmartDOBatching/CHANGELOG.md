# SmartDOBatching Changelog

## [1.0.0] - 2026-11-26
- Initial release.
- Hooks `CanVehicleTakeDistTask` and resolves `GetVehicleResourceCapacity` via AOB signatures for exact per-vehicle, per-resource capacity gating.
- Configurable Min Truck Fill % (default 50%) and Min Train Fill % (default 60%).
- Exempt Essential Goods option (Food, Meat, Alcohol, Medicine/Chemicals, Coal, Water, Fuel).
- Prevent Small-Building Deadlock option (caps required batch at 85% of destination storage).
- Quick presets: Vanilla 0%, 50%, 75%, 90%.
- F9 overlay "DO Batching" tab via shared mod_overlay hub.
- Persists settings in SmartDOBatching.ini alongside the DLL.
