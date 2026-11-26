# Changelog — MediaRedirector

All notable changes to **MediaRedirector** are documented here. The topmost section is automatically included in Steam Workshop release notes and the Workshop page description.

## [1.0.0]
- **Initial Release:** Added `MediaRedirector.dll` (RML API v1 & TesmioLoader API v4 compatible) providing a non-destructive Virtual File System (VFS) path redirector for `media_soviet\` files.
- **Synchronous Early-Boot Indexing:** Scans `<GameDir>\media_overrides\<PackName>\` and subscribed Steam Workshop overrides (`media_soviet\` or `override\` subfolders) during `DllMain` before `SOVIET64.exe` parses core `.ini` definitions.
- **Zero-Allocation, Lock-Free Detours:** Hooks `CreateFileW`, `GetFileAttributesW`, and `GetFileAttributesExW` with an immutable post-init `std::wstring_view` lookup map and 512-wchar stack buffers (zero heap allocations and zero mutex locks on the hot path).
- **Unified `F9` Media VFS Tab:** Added pack priority ordering (`Move Up` / `Move Down`), per-pack `[Enabled]` toggles, live `Enable Runtime Redirection` toggle, collapsible `Conflict Inspector`, and live telemetry (`Indexed Files`, `Active Conflicts`, `Redirected File Opens`).
