#pragma once
// ============================================================================
// dx11_overlay.hpp — DirectX 11 Present/ResizeBuffers + WndProc ImGui overlay
// ============================================================================

namespace tpl::overlay {

bool install();
void setActiveSavePath(const wchar_t* path);

} // namespace tpl::overlay
