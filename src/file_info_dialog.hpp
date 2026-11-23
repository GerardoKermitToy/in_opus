#pragma once
#include "opus_decoder.hpp"
#include <windows.h>

namespace winamp_opus {
void ShowFileInfoDialog(HINSTANCE instance, HWND parent, const wchar_t* path, const TrackInfo& info,
                        int output_channels, const std::wstring& output_note,
                        int output_rate = 48000, int output_bits = 16);
HWND CreateUnifiedInfoPane(HINSTANCE instance, HWND parent, const TrackInfo& info);
} // namespace winamp_opus
