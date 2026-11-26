# CODispatchControl

Native x64 plugin for **Workers & Resources: Soviet Republic** (`SOVIET64.exe`) that optimizes Construction Office (CO) vehicle dispatch so COs match truck and bus sizes to actual construction demand instead of sending oversized vehicles for tiny remaining workloads.

## Features

- **Min Material Truck Fill % (`0%`–`100%`, default `50%`)**: Rejects oversized material trucks when `remaining_phase_material / vehicle_capacity` is below the threshold and a smaller capable truck exists in the CO (automatically exempts small final top-offs below `1.5t`).
- **Min Bus Workday Ratio (`0.0`–`2.0`, default `0.5`)**: Prevents dispatching large buses when `remaining_workdays < bus_capacity * ratio` if a smaller bus/microbus exists in the CO.
- **Max Trucks En Route per Site (`1`–`10`, default `2`)**: Caps simultaneous material trucks heading to the same construction site to prevent road-node traffic jams.
- **Mechanism Transport Never Blocked**: Excavators, bulldozers, cranes, and flatbed carrier dispatches pass through untouched.
- **Live F9 Overlay (`CO Dispatch` tab)**: Tune sliders, apply `Vanilla` / `Balanced` / `Strict` presets, and save to `CODispatchControl.ini`.
