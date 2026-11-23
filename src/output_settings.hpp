#pragma once
#include "opus_decoder.hpp"
#include <windows.h>
#include <string>

namespace winamp_opus {
inline constexpr wchar_t kSettingsKey[] = L"Software\\WinampOpus\\Input";
inline constexpr wchar_t kOutputModeValue[] = L"OutputMode";
inline constexpr OutputMode kDefaultOutputMode = OutputMode::SourceChannels;

OutputMode LoadOutputMode() noexcept;
bool SaveOutputMode(OutputMode mode) noexcept;
// Applies only on OK after a successful save. Changes take effect on next Play.
OutputMode ShowOutputConfigDialog(HINSTANCE instance, HWND parent, OutputMode current,
                                  const std::wstring& playback_status);
std::wstring OutputFormatText(int channels);
} // namespace winamp_opus
