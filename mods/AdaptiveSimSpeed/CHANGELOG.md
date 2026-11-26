# Changelog

## [1.0.0]
- Initial **Early Alpha** release of `AdaptiveSimSpeed`: hardware-adaptive fast-forward (`3x`–`15x`) via the unified `F9` (`WRSR Tweaks [F9]` -> `Sim Speed`) overlay.
- Hooks `PreSimUpdate` and scales `C3D_TIMER` simulation speed dynamically within unused frame budget `(1000 / target_min_fps) - render_reserve_ms` using EMA-smoothed `QueryPerformanceCounter` measurements.
- Includes `Only Boost >> (Fast-Forward)` toggle, quick presets (`Vanilla 3x`, `Balanced 6x`, `Turbo 10x`, `Uncapped 15x`), live telemetry, and `AdaptiveSimSpeed.ini` persistence.
