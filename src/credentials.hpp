// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <atomic>
#include <string>

namespace winamp_opus {
// Connection-scoped native prompt. Never reads/writes Credential Manager.
// Cancel()'s flag dismisses this prompt; secret buffers are wiped on all exits.
bool AskCredentials(HWND parent, bool proxy, const std::wstring& server,
    const std::atomic<bool>& cancelled, const std::atomic<bool>& expired,
    std::wstring& user, std::wstring& password, bool insecure_basic = false);
void ClearPassword(std::wstring& password) noexcept;
} // namespace winamp_opus
