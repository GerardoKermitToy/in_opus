#pragma once
#include "opus_decoder.hpp"
#include <windows.h>

namespace winamp_opus {
void ShowFileInfoDialog(HINSTANCE instance, HWND parent, const wchar_t* path, const TrackInfo& info,
                        int output_channels, const std::wstring& output_note);
} // namespace winamp_opus
