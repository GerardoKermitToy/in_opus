// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <cstddef>
#include <string>

namespace winamp_opus {
// Initialize/refresh on the host window's UI thread. Missing/old services leave
// English text. No host interfaces or resource handles survive initialization.
void InitializeLanguage(HWND host, HINSTANCE own_module) noexcept;
void ShutdownLanguage() noexcept;
// Owned copy; thread-safe against cache reset, and never calls Winamp. Unknown
// phrases and untranslated entries preserve the supplied English fallback.
std::wstring Localized(const wchar_t* fallback);
// UI-thread only: caption and direct static/button labels, never paths, tags,
// editable/read-only data fields, combo internals or foreign dialog templates.
void LocalizeDialog(HWND dialog);
struct LanguageStringEntry { unsigned id; const wchar_t* fallback; };
unsigned LanguageStringId(const wchar_t* fallback) noexcept;
const LanguageStringEntry* LanguageStrings(std::size_t& count) noexcept;
namespace language {
// Independent Opus module identity. Never use in_vorbis's module GUID.
inline constexpr GUID kModuleGuid =
    {0x5e2fdcea, 0xf625, 0x48ab, {0x8a, 0xc4, 0xc5, 0x0f, 0x09, 0x9e, 0x32, 0xb8}};
inline constexpr GUID kServiceGuid =
    {0x30aed4e5, 0xef10, 0x4277, {0x8d, 0x49, 0x27, 0xab, 0x55, 0x70, 0xe8, 0x91}};
inline constexpr int kServiceByGuid = 50;
inline constexpr unsigned kGuidStringId = 65535;
namespace Message {
inline constexpr int GetStringW = 11;
inline constexpr int StartLanguageSupport = 30;
}
}
}
