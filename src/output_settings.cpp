#include "output_settings.hpp"
#include "resource.h"
#include <array>
#include <cmath>
#include <cwchar>

namespace winamp_opus {
namespace {
DWORD ReadSetting(const wchar_t* name, DWORD fallback) noexcept {
    DWORD value = fallback, bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
        nullptr, &value, &bytes) != ERROR_SUCCESS) return fallback;
    return value;
}
}
PlaybackSettings LoadPlaybackSettings() noexcept {
    PlaybackSettings s;
    s.output_mode = static_cast<OutputMode>(ReadSetting(kOutputModeValue, 1));
    s.sample_rate = static_cast<int>(ReadSetting(L"SampleRate", 48000));
    s.bits = static_cast<int>(ReadSetting(L"Bits", 16));
    s.resample_quality = static_cast<int>(ReadSetting(L"ResampleQuality", 5));
    s.gain_mode = static_cast<GainMode>(ReadSetting(L"GainMode", 0));
    s.pre_gain_q8 = static_cast<int>(ReadSetting(L"PreGainQ8", 0));
    s.radio_gain_q8 = static_cast<int>(ReadSetting(L"RadioGainQ8", 0));
    s.target_lufs = static_cast<int>(ReadSetting(L"TargetLUFS", static_cast<DWORD>(-23)));
    s.dither = ReadSetting(L"Dither", 1) != 0;
    s.instant_bitrate = ReadSetting(L"InstantBitrate", 1) != 0;
    s.unified_info = ReadSetting(L"UnifiedInfo", 1) != 0;
    return ValidateSettings(s);
}
bool SavePlaybackSettings(const PlaybackSettings& input) noexcept {
    const auto s = ValidateSettings(input);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    struct Value { const wchar_t* name; DWORD value; };
    const Value values[]{
        {kOutputModeValue, static_cast<DWORD>(s.output_mode)},
        {L"SampleRate", static_cast<DWORD>(s.sample_rate)}, {L"Bits", static_cast<DWORD>(s.bits)},
        {L"ResampleQuality", static_cast<DWORD>(s.resample_quality)},
        {L"GainMode", static_cast<DWORD>(s.gain_mode)}, {L"PreGainQ8", static_cast<DWORD>(s.pre_gain_q8)},
        {L"RadioGainQ8", static_cast<DWORD>(s.radio_gain_q8)}, {L"TargetLUFS", static_cast<DWORD>(s.target_lufs)},
        {L"Dither", s.dither ? 1u : 0u}, {L"InstantBitrate", s.instant_bitrate ? 1u : 0u},
        {L"UnifiedInfo", s.unified_info ? 1u : 0u}};
    bool ok = true;
    for (const auto& v : values) if (RegSetValueExW(key, v.name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&v.value), sizeof(v.value)) != ERROR_SUCCESS) { ok = false; break; }
    RegCloseKey(key);
    return ok;
}
OutputMode LoadOutputMode() noexcept { return LoadPlaybackSettings().output_mode; }
bool SaveOutputMode(OutputMode mode) noexcept {
    auto s = LoadPlaybackSettings(); s.output_mode = mode;
    return SavePlaybackSettings(s);
}
std::wstring OutputFormatText(int channels, int rate, int bits) {
    std::wstring layout;
    if (channels == 1) layout = L"mono";
    else if (channels == 2) layout = L"stereo";
    else if (channels == 6) layout = L"5.1 (6 channels)";
    else if (channels == 8) layout = L"7.1 (8 channels)";
    else layout = std::to_wstring(channels) + L" channels";
    const auto rate_text = rate % 1000 == 0 ? std::to_wstring(rate / 1000) + L",000" : std::to_wstring(rate);
    return rate_text + L" Hz, " + std::to_wstring(bits) + L"-bit, " + layout;
}
namespace {
struct ConfigData { PlaybackSettings settings; const std::wstring& status; };
void Combo(HWND dialog, int id, const wchar_t* text, int value) {
    const auto control = GetDlgItem(dialog, id);
    const auto index = SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(control, CB_SETITEMDATA, index, value);
}
int Selection(HWND dialog, int id) noexcept {
    const auto control = GetDlgItem(dialog, id);
    const auto index = SendMessageW(control, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? -1 : static_cast<int>(SendMessageW(control, CB_GETITEMDATA, index, 0));
}
void SetDb(HWND dialog, int id, int q8) noexcept {
    wchar_t value[32]{}; swprintf_s(value, L"%.2f", q8 / 256.0);
    SetDlgItemTextW(dialog, id, value);
}
bool ReadDb(HWND dialog, int id, int& q8) noexcept {
    wchar_t value[64]{}, *end = nullptr;
    GetDlgItemTextW(dialog, id, value, 64);
    const auto db = wcstod(value, &end);
    if (end == value || *end || !std::isfinite(db) || db < -60 || db > 30) return false;
    q8 = static_cast<int>(std::lround(db * 256)); return true;
}
INT_PTR CALLBACK ConfigProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* data = reinterpret_cast<ConfigData*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<ConfigData*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto& s = data->settings;
        CheckRadioButton(dialog, IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE,
            s.output_mode == OutputMode::SourceChannels ? IDC_OUTPUT_SOURCE : IDC_OUTPUT_STEREO);
        for (int rate : {8000, 12000, 16000, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000}) {
            wchar_t text[16]{}; swprintf_s(text, L"%d", rate); Combo(dialog, IDC_PCM_RATE, text, rate);
        }
        SetDlgItemInt(dialog, IDC_PCM_RATE, s.sample_rate, FALSE);
        SendDlgItemMessageW(dialog, IDC_PCM_RATE, CB_LIMITTEXT, 6, 0);
        int index = 0;
        for (int bits : {8, 16, 24, 32}) {
            wchar_t text[16]{}; swprintf_s(text, L"%d-bit", bits); Combo(dialog, IDC_PCM_BITS, text, bits);
            if (bits == s.bits) SendDlgItemMessageW(dialog, IDC_PCM_BITS, CB_SETCURSEL, index, 0);
            ++index;
        }
        for (int quality = 0; quality <= 10; ++quality) {
            wchar_t text[16]{}; swprintf_s(text, L"%d", quality); Combo(dialog, IDC_RESAMPLE_QUALITY, text, quality);
        }
        SendDlgItemMessageW(dialog, IDC_RESAMPLE_QUALITY, CB_SETCURSEL, s.resample_quality, 0);
        index = 0;
        for (auto* label : {L"Header gain only (default)", L"Album gain", L"Track gain",
            L"Automatic: track on shuffle, album otherwise", L"Raw: ignore header and R128 gain"})
            Combo(dialog, IDC_GAIN_MODE, label, index++);
        SendDlgItemMessageW(dialog, IDC_GAIN_MODE, CB_SETCURSEL, static_cast<int>(s.gain_mode), 0);
        SetDb(dialog, IDC_PRE_GAIN, s.pre_gain_q8); SetDb(dialog, IDC_RADIO_GAIN, s.radio_gain_q8);
        SetDlgItemInt(dialog, IDC_TARGET_LUFS, s.target_lufs, TRUE);
        for (int id : {IDC_PRE_GAIN, IDC_RADIO_GAIN, IDC_TARGET_LUFS})
            SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 12, 0);
        CheckDlgButton(dialog, IDC_DITHER, s.dither ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_INSTANT_BR, s.instant_bitrate ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_UNIFIED_INFO, s.unified_info ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dialog, IDC_OUTPUT_STATUS, data->status.c_str());
        return TRUE;
    }
    if (message == WM_COMMAND && data) {
        if (LOWORD(wparam) == IDOK) {
            auto s = data->settings;
            s.output_mode = IsDlgButtonChecked(dialog, IDC_OUTPUT_SOURCE) == BST_CHECKED ? OutputMode::SourceChannels : OutputMode::Stereo;
            BOOL rate_ok = FALSE, lufs_ok = FALSE;
            s.sample_rate = GetDlgItemInt(dialog, IDC_PCM_RATE, &rate_ok, FALSE);
            s.target_lufs = static_cast<int>(GetDlgItemInt(dialog, IDC_TARGET_LUFS, &lufs_ok, TRUE));
            s.bits = Selection(dialog, IDC_PCM_BITS);
            s.resample_quality = Selection(dialog, IDC_RESAMPLE_QUALITY);
            s.gain_mode = static_cast<GainMode>(Selection(dialog, IDC_GAIN_MODE));
            if (!rate_ok || s.sample_rate < 8000 || s.sample_rate > 192000 || !lufs_ok ||
                s.target_lufs < -40 || s.target_lufs > 0 || !ReadDb(dialog, IDC_PRE_GAIN, s.pre_gain_q8) ||
                !ReadDb(dialog, IDC_RADIO_GAIN, s.radio_gain_q8)) {
                MessageBoxW(dialog, L"Use a rate from 8,000 to 192,000 Hz, gains from -60 to +30 dB, and a target from -40 to 0 LUFS.",
                    L"Invalid audio settings", MB_OK | MB_ICONWARNING); return TRUE;
            }
            s.dither = IsDlgButtonChecked(dialog, IDC_DITHER) == BST_CHECKED;
            s.instant_bitrate = IsDlgButtonChecked(dialog, IDC_INSTANT_BR) == BST_CHECKED;
            s.unified_info = IsDlgButtonChecked(dialog, IDC_UNIFIED_INFO) == BST_CHECKED;
            if (!SavePlaybackSettings(s)) {
                MessageBoxW(dialog, L"Could not save settings for the current user.", L"Winamp Opus Input", MB_OK | MB_ICONERROR); return TRUE;
            }
            data->settings = ValidateSettings(s); EndDialog(dialog, IDOK); return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
}
PlaybackSettings ShowPlaybackConfigDialog(HINSTANCE instance, HWND parent, PlaybackSettings current,
    const std::wstring& playback_status) {
    ConfigData data{current, playback_status};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_CONFIG), parent, ConfigProc, reinterpret_cast<LPARAM>(&data));
    if (result == -1) MessageBoxW(parent, L"Could not open the plugin settings dialog.", L"Winamp Opus Input", MB_OK | MB_ICONERROR);
    return result == IDOK ? data.settings : current;
}
OutputMode ShowOutputConfigDialog(HINSTANCE instance, HWND parent, OutputMode current, const std::wstring& status) {
    auto s = LoadPlaybackSettings(); s.output_mode = current;
    return ShowPlaybackConfigDialog(instance, parent, s, status).output_mode;
}
} // namespace winamp_opus
