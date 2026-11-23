#pragma once
#include "opus_decoder.hpp"
#include <windows.h>
#include <string>

namespace winamp_opus {
inline constexpr wchar_t kSettingsKey[] = L"Software\\WinampOpus\\Input";
inline constexpr wchar_t kOutputModeValue[] = L"OutputMode";
inline constexpr OutputMode kDefaultOutputMode = OutputMode::SourceChannels;
PlaybackSettings LoadPlaybackSettings() noexcept;
bool SavePlaybackSettings(const PlaybackSettings& settings) noexcept;
PlaybackSettings ShowPlaybackConfigDialog(HINSTANCE instance, HWND parent,
    PlaybackSettings current, const std::wstring& playback_status);
// Compatibility helpers for existing callers; keep all other settings intact.
OutputMode LoadOutputMode() noexcept;
bool SaveOutputMode(OutputMode mode) noexcept;
OutputMode ShowOutputConfigDialog(HINSTANCE instance, HWND parent, OutputMode current,
                                  const std::wstring& playback_status);
std::wstring OutputFormatText(int channels, int rate = 48000, int bits = 16);
} // namespace winamp_opus
