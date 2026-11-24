// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "opus_decoder.hpp"
namespace winamp_opus {
// Advanced child only: the host owns Basic Info, Artwork, tabs and OK/Cancel.
HWND CreateTagEditorPane(HINSTANCE module, HWND parent, const wchar_t* path, const TrackInfo& info);
}
