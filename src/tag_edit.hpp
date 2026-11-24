// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tag_writer.hpp"
#include <windows.h>

namespace winamp_opus {
struct TrackInfo;
// Convert one Basic Info edit to native fields, including rating/R128 and aliases.
bool PrepareTagPatch(const TrackInfo&, const char* key, const wchar_t* value, OpusTagChanges&);
// Use the same metadata-overlay/notification path as the setter and fallback editor.
void NotifyTagsCommitted(const wchar_t* path) noexcept;
// Callback runs after a confirmed, non-no-op commit, outside all staging locks.
// It must invalidate/signal only; it must not operate on the playback decoder.
void ConfigureTagEditing(HWND host, void (*invalidate)(const wchar_t*)) noexcept;
// Resource-independent native editor. Returns true after Save (also for no-op),
// false on cancellation or failure. All links are initially selected.
bool EditOpusTags(HINSTANCE instance, HWND parent, const wchar_t* path);
} // namespace winamp_opus
