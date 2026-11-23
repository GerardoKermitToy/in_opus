#include "output_settings.hpp"
#include "resource.h"

namespace winamp_opus {
OutputMode LoadOutputMode() noexcept {
    DWORD value = 0, bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kOutputModeValue,
            RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return kDefaultOutputMode;
    if (value == static_cast<DWORD>(OutputMode::Stereo)) return OutputMode::Stereo;
    return kDefaultOutputMode;
}
bool SaveOutputMode(OutputMode mode) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = static_cast<DWORD>(mode);
    const auto result = RegSetValueExW(key, kOutputModeValue, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::wstring OutputFormatText(int channels) {
    std::wstring layout;
    if (channels == 1) layout = L"mono";
    else if (channels == 2) layout = L"stereo";
    else if (channels == 6) layout = L"5.1 (6 channels)";
    else if (channels == 8) layout = L"7.1 (8 channels)";
    else layout = std::to_wstring(channels) + L" channels";
    return L"48,000 Hz, 16-bit, " + layout;
}

namespace {
struct ConfigData {
    OutputMode mode;
    const std::wstring& status;
};
INT_PTR CALLBACK ConfigProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* data = reinterpret_cast<ConfigData*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<ConfigData*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        CheckRadioButton(dialog, IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE,
            data->mode == OutputMode::SourceChannels ? IDC_OUTPUT_SOURCE : IDC_OUTPUT_STEREO);
        SetDlgItemTextW(dialog, IDC_OUTPUT_STATUS, data->status.c_str());
        return TRUE;
    }
    if (message == WM_COMMAND && data) {
        if (LOWORD(wparam) == IDOK) {
            const auto selected = IsDlgButtonChecked(dialog, IDC_OUTPUT_SOURCE) == BST_CHECKED
                ? OutputMode::SourceChannels : OutputMode::Stereo;
            if (!SaveOutputMode(selected)) {
                MessageBoxW(dialog, L"Could not save settings for the current user.",
                    L"Winamp Opus Input", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            data->mode = selected;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
}

OutputMode ShowOutputConfigDialog(HINSTANCE instance, HWND parent, OutputMode current,
                                  const std::wstring& playback_status) {
    ConfigData data{current, playback_status};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_CONFIG), parent,
        ConfigProc, reinterpret_cast<LPARAM>(&data));
    if (result == -1) {
        MessageBoxW(parent, L"Could not open the plugin settings dialog.",
            L"Winamp Opus Input", MB_OK | MB_ICONERROR);
    }
    return result == IDOK ? data.mode : current;
}
} // namespace winamp_opus
