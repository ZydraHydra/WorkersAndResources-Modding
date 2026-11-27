#pragma once
// ============================================================================
// save_tracker.hpp — CreateFileW IAT hook to track active save directory
// ============================================================================

namespace tpl::save_tracker {

bool install();
void handleSaveLoad(const wchar_t* saveDir);
void handleSaveWrite(const wchar_t* saveDir);

} // namespace tpl::save_tracker
