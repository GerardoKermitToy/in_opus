#pragma once
#include "fixtures.hpp"
#include "output_settings.hpp"
#include <windows.h>
#include <string>

// Redirect HKCU in this test process, including the loaded DLL. Never read or
// change the user's real plugin settings; each CTest process has its own key.
class RegistrySandbox final {
public:
    RegistrySandbox() {
        path_ = L"Software\\WinampOpusTests\\" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        DWORD disposition = 0;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &key_, &disposition) == ERROR_SUCCESS, "create test settings sandbox");
        Check(disposition == REG_CREATED_NEW_KEY, "test settings sandbox must be new");
        if (RegOverridePredefKey(HKEY_CURRENT_USER, key_) != ERROR_SUCCESS) {
            RegCloseKey(key_);
            key_ = nullptr;
            RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
            Check(false, "redirect test settings");
        }
    }
    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key_) RegCloseKey(key_);
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }
    void StoreOutputMode(DWORD value) {
        HKEY key = nullptr;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, winamp_opus::kSettingsKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create sandbox output setting");
        const auto result = RegSetValueExW(key, winamp_opus::kOutputModeValue, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
        Check(result == ERROR_SUCCESS, "store sandbox output setting");
    }
    RegistrySandbox(const RegistrySandbox&) = delete;
    RegistrySandbox& operator=(const RegistrySandbox&) = delete;
private:
    HKEY key_ = nullptr;
    std::wstring path_;
};
