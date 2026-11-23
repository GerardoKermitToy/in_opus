#include "output_settings.hpp"
#include "resource.h"
#include "language.hpp"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <type_traits>
#include <utility>

namespace winamp_opus {
namespace {
// Explicit symmetric catalogue, retaining every existing registry value name.
#define OPUS_DWORD_SETTINGS(X) \
    X(output_mode, L"OutputMode") X(sample_rate, L"SampleRate") \
    X(bits, L"Bits") X(resample_quality, L"ResampleQuality") \
    X(gain_mode, L"GainMode") X(pre_gain_q8, L"PreGainQ8") \
    X(radio_gain_q8, L"RadioGainQ8") X(target_lufs, L"TargetLUFS") \
    X(dither, L"Dither") X(instant_bitrate, L"InstantBitrate") X(unified_info, L"UnifiedInfo") \
    X(opus_hd, L"OpusHd") X(opus_bwe, L"OpusBwe") X(speech, L"SpeechMode") \
    X(mono_compatible, L"MonoCompatible") X(ignore_extensions, L"IgnoreExtensions") \
    X(ambisonic, L"AmbisonicMode") X(recovery, L"RecoveryMode") X(recovery_limit_ms, L"RecoveryLimitMs") \
    X(dither_pdf, L"DitherPdf") X(noise_shape, L"NoiseShape") X(custom_quantizer, L"CustomQuantizer") \
    X(use_host_replaygain, L"UseHostReplayGain") X(use_host_proxy, L"UseHostProxy") \
    X(preferred_gain_only, L"PreferredGainOnly") X(prevent_clipping, L"PreventClipping") \
    X(peak_mode, L"PeakMode") X(non_rg_q8, L"NonReplayGainQ8") X(buffer_small_files, L"BufferSmallFiles") \
    X(timeout_ms, L"HttpTimeoutMs") X(probe_kb, L"ProbeLimitKb") \
    X(reconnect_attempts, L"ReconnectAttempts") X(reconnect_delay_ms, L"ReconnectDelayMs") \
    X(prebuffer_kb, L"PrebufferKb") X(stream_buffer_kb, L"StreamBufferKb") X(underrun_percent, L"UnderrunPercent") \
    X(save_streams, L"SaveStreams") X(interactive_auth, L"InteractiveAuth") X(http_seek, L"HttpSeek") X(proxy_mode, L"ProxyMode")
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
};
template<class T> void ReadSetting(HKEY key, const wchar_t* name, T& output) noexcept {
    DWORD value = 0, bytes = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(value)) return;
    if constexpr (std::is_same_v<T, bool>) {
        if (value <= 1) output = value != 0;
    } else output = static_cast<T>(static_cast<LONG>(value));
}
void ReadString(HKEY key, const wchar_t* name, std::size_t maximum, std::wstring& output) {
    DWORD bytes = 0, type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) ||
        bytes > (maximum + 1) * sizeof(wchar_t)) return;
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    DWORD actual = bytes;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &actual) != ERROR_SUCCESS ||
        type != REG_SZ || actual != bytes || buffer.back() != L'\0' ||
        buffer.find(L'\0') != buffer.size() - 1) return;
    buffer.pop_back();
    output = std::move(buffer);
}
bool WriteString(HKEY key, const wchar_t* name, const std::wstring& text) noexcept {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
bool ValidDirectory(const std::wstring& text) noexcept {
    return text.size() <= 32767 && std::none_of(text.begin(), text.end(), [](wchar_t c) { return c < 32; });
}
bool ValidProxy(const std::wstring& text) noexcept {
    if (text.empty()) return true;
    if (text.size() > 2048) return false;
    // One credential-free HTTP endpoint, not a proxy/header list or PAC URL.
    std::size_t first = 0, last = text.size();
    if (last >= 7 && _wcsnicmp(text.c_str(), L"http://", 7) == 0) {
        first = 7;
        if (text.back() == L'/') --last;
    }
    if (first == last) return false;
    for (std::size_t i = first; i < last; ++i) {
        const auto c = text[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L':' || c == L'[' || c == L']')) return false;
    }
    auto host_end = first;
    if (text[first] == L'[') {
        const auto close = text.find(L']', first + 1);
        if (close == std::wstring::npos || close >= last || close == first + 1) return false;
        unsigned colons = 0;
        for (auto i = first + 1; i < close; ++i) {
            const auto c = text[i];
            if (c == L':') ++colons;
            else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                (c >= L'A' && c <= L'F') || c == L'.')) return false;
        }
        if (colons < 2) return false;
        host_end = close + 1;
    } else {
        while (host_end < last && text[host_end] != L':') {
            const auto c = text[host_end++];
            if (c == L'[' || c == L']') return false;
        }
        if (host_end == first || host_end - first > 253 || text[first] == L'.' || text[first] == L'-' ||
            text[host_end - 1] == L'-') return false;
        for (auto i = first + 1; i < host_end; ++i)
            if (text[i] == L'.' && text[i - 1] == L'.') return false;
    }
    if (host_end == last) return true;
    if (text[host_end] != L':' || ++host_end == last || last - host_end > 5) return false;
    unsigned port = 0;
    for (auto i = host_end; i < last; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return false;
        port = port * 10 + static_cast<unsigned>(text[i] - L'0');
    }
    return port > 0 && port <= 65535;
}
void SanitizeStrings(PlaybackSettings& settings) noexcept {
    if (!ValidProxy(settings.proxy_url)) settings.proxy_url.clear();
    if (!ValidDirectory(settings.save_directory)) settings.save_directory.clear();
}
}
PlaybackSettings LoadPlaybackSettings() noexcept {
    try {
        PlaybackSettings s;
        RegistryKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key.value) == ERROR_SUCCESS) {
#define READ_MEMBER(member, name) ReadSetting(key.value, name, s.member);
            OPUS_DWORD_SETTINGS(READ_MEMBER)
#undef READ_MEMBER
            ReadString(key.value, L"ProxyUrl", 2048, s.proxy_url);
            ReadString(key.value, L"SaveDirectory", 32767, s.save_directory);
        }
        SanitizeStrings(s);
        return ValidateSettings(std::move(s));
    } catch (...) { return PlaybackSettings{}; }
}
bool SavePlaybackSettings(const PlaybackSettings& input) noexcept {
    try {
        auto s = ValidateSettings(input);
        SanitizeStrings(s);
        RegistryKey key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key.value, nullptr) != ERROR_SUCCESS) return false;
        struct Value { const wchar_t* name; DWORD value; };
#define WRITE_MEMBER(member, name) {name, static_cast<DWORD>(s.member)},
        const Value values[]{OPUS_DWORD_SETTINGS(WRITE_MEMBER)};
#undef WRITE_MEMBER
        for (const auto& v : values) if (RegSetValueExW(key.value, v.name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&v.value), sizeof(v.value)) != ERROR_SUCCESS) return false;
        return WriteString(key.value, L"ProxyUrl", s.proxy_url) && WriteString(key.value, L"SaveDirectory", s.save_directory);
    } catch (...) { return false; }
}
#undef OPUS_DWORD_SETTINGS
OutputMode LoadOutputMode() noexcept { return LoadPlaybackSettings().output_mode; }
bool SaveOutputMode(OutputMode mode) noexcept {
    auto s = LoadPlaybackSettings(); s.output_mode = mode;
    return SavePlaybackSettings(s);
}
std::wstring OutputFormatText(int channels, int rate, int bits) {
    std::wstring layout;
    if (channels == 1) layout = Localized(L"mono");
    else if (channels == 2) layout = Localized(L"stereo");
    else if (channels == 6) layout = Localized(L"5.1 (6 channels)");
    else if (channels == 8) layout = Localized(L"7.1 (8 channels)");
    else layout = std::to_wstring(channels) + Localized(L" channels");
    const auto rate_text = rate % 1000 == 0 ? std::to_wstring(rate / 1000) + L",000" : std::to_wstring(rate);
    return (rate == 0 ? Localized(L"native rate") : rate_text + L" Hz") + L", " + std::to_wstring(bits) + L"-bit, " + layout;
}
namespace {
struct ConfigData { PlaybackSettings settings; const std::wstring& status; };
int Prompt(HWND dialog, const wchar_t* text, const wchar_t* title, UINT flags) {
    const auto message = Localized(text), caption = Localized(title);
    return MessageBoxW(dialog, message.c_str(), caption.c_str(), flags);
}
template<auto Proc> INT_PTR CALLBACK ModalGuard(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try { return Proc(dialog, message, wparam, lparam); }
    catch (...) { EndDialog(dialog, IDCANCEL); return TRUE; }
}
void Combo(HWND dialog, int id, const wchar_t* text, int value) {
    const auto control = GetDlgItem(dialog, id);
    const auto label = Localized(text);
    const auto index = SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (index != CB_ERR && index != CB_ERRSPACE) SendMessageW(control, CB_SETITEMDATA, index, value);
}
void Choices(HWND dialog, int id, std::initializer_list<const wchar_t*> labels, int selection) {
    int index = 0;
    for (const auto* label : labels) Combo(dialog, id, label, index++);
    SendDlgItemMessageW(dialog, id, CB_SETCURSEL, selection, 0);
}
int Selection(HWND dialog, int id) noexcept {
    const auto control = GetDlgItem(dialog, id);
    const auto index = SendMessageW(control, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? -1 : static_cast<int>(SendMessageW(control, CB_GETITEMDATA, index, 0));
}
void SetDb(HWND dialog, int id, int q8) noexcept {
    wchar_t value[32]{}; swprintf_s(value, L"%.3f", q8 / 256.0);
    SetDlgItemTextW(dialog, id, value);
    SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 12, 0);
}
bool ReadDb(HWND dialog, int id, int& q8) noexcept {
    wchar_t value[64]{}, *end = nullptr;
    GetDlgItemTextW(dialog, id, value, 64);
    const auto db = wcstod(value, &end);
    if (end == value || *end || !std::isfinite(db) || db < -60 || db > 30) return false;
    q8 = static_cast<int>(std::lround(db * 256)); return true;
}
void Check(HWND dialog, int id, bool checked) noexcept { CheckDlgButton(dialog, id, checked ? BST_CHECKED : BST_UNCHECKED); }
bool Checked(HWND dialog, int id) noexcept { return IsDlgButtonChecked(dialog, id) == BST_CHECKED; }
void Number(HWND dialog, int id, int value) noexcept {
    SetDlgItemInt(dialog, id, value, FALSE);
    SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 6, 0);
}
bool ReadNumber(HWND dialog, int id, int minimum, int maximum, int& value) noexcept {
    BOOL valid = FALSE;
    const auto number = GetDlgItemInt(dialog, id, &valid, TRUE);
    if (!valid || static_cast<int>(number) < minimum || static_cast<int>(number) > maximum) return false;
    value = static_cast<int>(number); return true;
}
std::wstring ReadText(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    const int length = GetWindowTextLengthW(control);
    if (length < 0 || length > 32767) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const auto copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}
INT_PTR AdvancedProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* settings = reinterpret_cast<PlaybackSettings*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        settings = reinterpret_cast<PlaybackSettings*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        Check(dialog, IDC_OPUS_HD, settings->opus_hd); Check(dialog, IDC_OPUS_BWE, settings->opus_bwe);
        Check(dialog, IDC_MONO_COMPATIBLE, settings->mono_compatible); Check(dialog, IDC_IGNORE_EXTENSIONS, settings->ignore_extensions);
        Choices(dialog, IDC_OPUS_SPEECH, {L"Off (standard decoding)", L"LACE (complexity 6)", L"NoLACE (complexity 7)"}, static_cast<int>(settings->speech));
        Choices(dialog, IDC_AMBISONIC_MODE, {L"Stereo speakers", L"Headphones (analytic approximate binaural)", L"Quad speakers",
            L"5.1 speakers (silent LFE)", L"7.1 speakers (silent LFE)"}, static_cast<int>(settings->ambisonic));
        Choices(dialog, IDC_RECOVERY_MODE, {L"Strict (stop on later loss; default)", L"Basic packet-loss concealment (PLC)",
            L"Neural PLC", L"FEC, then neural PLC", L"DRED, then FEC / neural PLC"}, static_cast<int>(settings->recovery));
        Number(dialog, IDC_RECOVERY_LIMIT, settings->recovery_limit_ms);
        LocalizeDialog(dialog); return TRUE;
    }
    if (message == WM_COMMAND && settings) {
        if (LOWORD(wparam) == IDOK) {
            auto s = *settings;
            if (!ReadNumber(dialog, IDC_RECOVERY_LIMIT, 20, 1000, s.recovery_limit_ms)) {
                Prompt(dialog, L"Use a recovery limit from 20 to 1000 ms.", L"Invalid recovery limit", MB_OK | MB_ICONWARNING); return TRUE;
            }
            s.opus_hd = Checked(dialog, IDC_OPUS_HD); s.opus_bwe = Checked(dialog, IDC_OPUS_BWE);
            s.mono_compatible = Checked(dialog, IDC_MONO_COMPATIBLE); s.ignore_extensions = Checked(dialog, IDC_IGNORE_EXTENSIONS);
            s.speech = static_cast<SpeechMode>(Selection(dialog, IDC_OPUS_SPEECH));
            s.ambisonic = static_cast<AmbisonicMode>(Selection(dialog, IDC_AMBISONIC_MODE));
            s.recovery = static_cast<RecoveryMode>(Selection(dialog, IDC_RECOVERY_MODE));
            *settings = ValidateSettings(std::move(s)); EndDialog(dialog, IDOK); return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
struct MoreData {
    PlaybackSettings draft;
    std::array<HWND, 3> pages{};
    int page = 0;
};
void QuantizerControls(HWND page) noexcept {
    const BOOL enabled = Checked(page, IDC_CUSTOM_QUANTIZER);
    EnableWindow(GetDlgItem(page, IDC_DITHER_PDF), enabled);
    EnableWindow(GetDlgItem(page, IDC_NOISE_SHAPE), enabled);
}
INT_PTR CALLBACK PageProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
        if (message == WM_INITDIALOG) {
            const auto& s = *reinterpret_cast<const PlaybackSettings*>(lparam);
            if (GetDlgItem(dialog, IDC_CUSTOM_QUANTIZER)) {
                Check(dialog, IDC_CUSTOM_QUANTIZER, s.custom_quantizer);
                Choices(dialog, IDC_DITHER_PDF, {L"Rectangular", L"Triangular", L"Gaussian"}, static_cast<int>(s.dither_pdf));
                Choices(dialog, IDC_NOISE_SHAPE, {L"None", L"First-order", L"ATH psychoacoustic", L"ATH (low amplitude)"}, static_cast<int>(s.noise_shape));
                Check(dialog, IDC_BUFFER_SMALL_FILES, s.buffer_small_files); QuantizerControls(dialog);
            } else if (GetDlgItem(dialog, IDC_HOST_REPLAYGAIN)) {
                Check(dialog, IDC_HOST_REPLAYGAIN, s.use_host_replaygain); Check(dialog, IDC_PREFERRED_GAIN_ONLY, s.preferred_gain_only);
                Check(dialog, IDC_PREVENT_CLIPPING, s.prevent_clipping);
                Choices(dialog, IDC_PEAK_MODE, {L"Apply gain (allow clipping)", L"Apply gain with peak protection",
                    L"Normalize using tagged peak", L"Prevent clipping only"}, s.peak_mode);
                SetDb(dialog, IDC_NON_RG_GAIN, s.non_rg_q8);
            } else {
                Check(dialog, IDC_HOST_PROXY, s.use_host_proxy);
                Number(dialog, IDC_TIMEOUT_MS, s.timeout_ms); Number(dialog, IDC_PROBE_KB, s.probe_kb);
                Number(dialog, IDC_RECONNECT_ATTEMPTS, s.reconnect_attempts); Number(dialog, IDC_RECONNECT_DELAY, s.reconnect_delay_ms);
                Number(dialog, IDC_STREAM_BUFFER, s.stream_buffer_kb); Number(dialog, IDC_PREBUFFER_KB, s.prebuffer_kb);
                Number(dialog, IDC_UNDERRUN_PERCENT, s.underrun_percent);
                Choices(dialog, IDC_PROXY_MODE, {L"Never", L"HTTP port 80 only", L"Always"}, s.proxy_mode);
                SetDlgItemTextW(dialog, IDC_PROXY_URL, s.proxy_url.c_str());
                SendDlgItemMessageW(dialog, IDC_PROXY_URL, EM_SETLIMITTEXT, 2048, 0);
                Check(dialog, IDC_SAVE_STREAMS, s.save_streams); Check(dialog, IDC_INTERACTIVE_AUTH, s.interactive_auth);
                Check(dialog, IDC_HTTP_SEEK, s.http_seek);
                SetDlgItemTextW(dialog, IDC_SAVE_DIRECTORY, s.save_directory.c_str());
                SendDlgItemMessageW(dialog, IDC_SAVE_DIRECTORY, EM_SETLIMITTEXT, 32767, 0);
            }
            LocalizeDialog(dialog); return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_CUSTOM_QUANTIZER) { QuantizerControls(dialog); return TRUE; }
        if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
            SendMessageW(GetParent(dialog), WM_COMMAND, LOWORD(wparam), 0); return TRUE;
        }
    } catch (...) { EndDialog(GetParent(dialog), IDCANCEL); }
    return FALSE;
}
void SelectPage(HWND dialog, MoreData& data, int index, bool focus) noexcept {
    if (index < 0 || index >= static_cast<int>(data.pages.size())) return;
    TabCtrl_SetCurSel(GetDlgItem(dialog, IDC_SETTINGS_TABS), index);
    for (int i = 0; i < static_cast<int>(data.pages.size()); ++i) ShowWindow(data.pages[i], i == index ? SW_SHOW : SW_HIDE);
    data.page = index;
    if (focus) SetFocus(GetNextDlgTabItem(data.pages[index], nullptr, FALSE));
}
bool ReadMore(HWND dialog, MoreData& data) {
    auto s = data.draft;
    auto page = data.pages[0];
    s.custom_quantizer = Checked(page, IDC_CUSTOM_QUANTIZER);
    s.dither_pdf = static_cast<DitherPdf>(Selection(page, IDC_DITHER_PDF));
    s.noise_shape = static_cast<NoiseShape>(Selection(page, IDC_NOISE_SHAPE));
    s.buffer_small_files = Checked(page, IDC_BUFFER_SMALL_FILES);
    if (static_cast<unsigned>(s.dither_pdf) > 2 || static_cast<unsigned>(s.noise_shape) > 3) {
        SelectPage(dialog, data, 0, true);
        Prompt(dialog, L"Select a valid dither distribution and noise shaping mode.", L"Invalid processing settings", MB_OK | MB_ICONWARNING); return false;
    }
    page = data.pages[1];
    s.use_host_replaygain = Checked(page, IDC_HOST_REPLAYGAIN); s.preferred_gain_only = Checked(page, IDC_PREFERRED_GAIN_ONLY);
    s.prevent_clipping = Checked(page, IDC_PREVENT_CLIPPING); s.peak_mode = Selection(page, IDC_PEAK_MODE);
    if (s.peak_mode < 0 || s.peak_mode > 3 || !ReadDb(page, IDC_NON_RG_GAIN, s.non_rg_q8)) {
        SelectPage(dialog, data, 1, true);
        Prompt(dialog, L"Select a peak mode and use an untagged gain from -60 to +30 dB.", L"Invalid gain settings", MB_OK | MB_ICONWARNING); return false;
    }
    page = data.pages[2];
    s.use_host_proxy = Checked(page, IDC_HOST_PROXY); s.proxy_mode = Selection(page, IDC_PROXY_MODE);
    if (!ReadNumber(page, IDC_TIMEOUT_MS, 1000, 30000, s.timeout_ms) || !ReadNumber(page, IDC_PROBE_KB, 64, 4096, s.probe_kb) ||
        !ReadNumber(page, IDC_RECONNECT_ATTEMPTS, 0, 10, s.reconnect_attempts) ||
        !ReadNumber(page, IDC_RECONNECT_DELAY, 250, 30000, s.reconnect_delay_ms) ||
        !ReadNumber(page, IDC_STREAM_BUFFER, 32, 4096, s.stream_buffer_kb) ||
        !ReadNumber(page, IDC_PREBUFFER_KB, 0, std::min(s.stream_buffer_kb, s.probe_kb / 2), s.prebuffer_kb) ||
        !ReadNumber(page, IDC_UNDERRUN_PERCENT, 0, 100, s.underrun_percent) || s.proxy_mode < 0 || s.proxy_mode > 2) {
        SelectPage(dialog, data, 2, true);
        Prompt(dialog, L"Use timeout 1000-30000 ms, probe 64-4096 KiB, retries 0-10, delay 250-30000 ms, buffer 32-4096 KiB, prebuffer no larger than buffer or half the probe, and rebuffer threshold 0-100%.",
            L"Invalid network settings", MB_OK | MB_ICONWARNING); return false;
    }
    s.proxy_url = ReadText(page, IDC_PROXY_URL); s.save_directory = ReadText(page, IDC_SAVE_DIRECTORY);
    if (!ValidProxy(s.proxy_url) || !ValidDirectory(s.save_directory)) {
        SelectPage(dialog, data, 2, true);
        Prompt(dialog, L"Enter a single credential-free HTTP proxy endpoint and a save path without control characters.", L"Invalid network settings", MB_OK | MB_ICONWARNING); return false;
    }
    s.save_streams = Checked(page, IDC_SAVE_STREAMS); s.interactive_auth = Checked(page, IDC_INTERACTIVE_AUTH);
    s.http_seek = Checked(page, IDC_HTTP_SEEK);
    data.draft = ValidateSettings(std::move(s)); return true;
}
INT_PTR MoreProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* data = reinterpret_cast<MoreData*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<MoreData*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto tabs = GetDlgItem(dialog, IDC_SETTINGS_TABS);
        int index = 0;
        for (const auto* label : {L"Processing", L"Gain / host", L"Network"}) {
            auto text = Localized(label); TCITEMW tab{}; tab.mask = TCIF_TEXT; tab.pszText = text.data();
            TabCtrl_InsertItem(tabs, index++, &tab);
        }
        RECT rect{}; GetClientRect(tabs, &rect); TabCtrl_AdjustRect(tabs, FALSE, &rect);
        MapWindowPoints(tabs, dialog, reinterpret_cast<POINT*>(&rect), 2);
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
        const int resources[]{IDD_OPUS_PROCESSING, IDD_OPUS_GAIN_OPTIONS, IDD_OPUS_NETWORK};
        for (int i = 0; i < 3; ++i) {
            data->pages[i] = CreateDialogParamW(instance, MAKEINTRESOURCEW(resources[i]), dialog, PageProc, reinterpret_cast<LPARAM>(&data->draft));
            if (!data->pages[i]) { EndDialog(dialog, -1); return TRUE; }
            SetWindowPos(data->pages[i], tabs, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
        }
        SelectPage(dialog, *data, 0, false); LocalizeDialog(dialog); return TRUE;
    }
    if (message == WM_NOTIFY && data) {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == IDC_SETTINGS_TABS && header->code == TCN_SELCHANGE) {
            SelectPage(dialog, *data, TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_SETTINGS_TABS)), false); return TRUE;
        }
    }
    if (message == WM_COMMAND && data) {
        if (LOWORD(wparam) == IDOK) { if (ReadMore(dialog, *data)) EndDialog(dialog, IDOK); return TRUE; }
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
void SyncRadios(HWND dialog, OutputMode mode) noexcept {
    CheckRadioButton(dialog, IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE,
        mode == OutputMode::SourceChannels ? IDC_OUTPUT_SOURCE : mode == OutputMode::Stereo ? IDC_OUTPUT_STEREO : 0);
}
INT_PTR ConfigProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* data = reinterpret_cast<ConfigData*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<ConfigData*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto& s = data->settings;
        SyncRadios(dialog, s.output_mode);
        Choices(dialog, IDC_OUTPUT_MODE, {L"Stereo", L"Source (Windows channel order)", L"Mono", L"Quad (downmix 5.1+)",
            L"Source (unmapped codec channel order)", L"Stereo surround matrix (DS)", L"Stereo surround matrix (DS2)"}, static_cast<int>(s.output_mode));
        for (int rate : {0, 8000, 12000, 16000, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000}) {
            wchar_t text[16]{}; swprintf_s(text, L"%d", rate); Combo(dialog, IDC_PCM_RATE, text, rate);
        }
        SetDlgItemInt(dialog, IDC_PCM_RATE, s.sample_rate, FALSE); SendDlgItemMessageW(dialog, IDC_PCM_RATE, CB_LIMITTEXT, 6, 0);
        int index = 0;
        for (const auto* label : {L"8-bit", L"16-bit", L"24-bit", L"32-bit"}) {
            const int bits[]{8, 16, 24, 32}; Combo(dialog, IDC_PCM_BITS, label, bits[index]);
            if (bits[index] == s.bits) SendDlgItemMessageW(dialog, IDC_PCM_BITS, CB_SETCURSEL, index, 0);
            ++index;
        }
        for (int quality = 0; quality <= 10; ++quality) {
            wchar_t text[16]{}; swprintf_s(text, L"%d", quality); Combo(dialog, IDC_RESAMPLE_QUALITY, text, quality);
        }
        SendDlgItemMessageW(dialog, IDC_RESAMPLE_QUALITY, CB_SETCURSEL, s.resample_quality, 0);
        Choices(dialog, IDC_GAIN_MODE, {L"Header gain only (default)", L"Album gain", L"Track gain",
            L"Automatic: track on shuffle, album otherwise", L"Raw: ignore header and R128 gain"}, static_cast<int>(s.gain_mode));
        SetDb(dialog, IDC_PRE_GAIN, s.pre_gain_q8); SetDb(dialog, IDC_RADIO_GAIN, s.radio_gain_q8);
        SetDlgItemInt(dialog, IDC_TARGET_LUFS, s.target_lufs, TRUE); SendDlgItemMessageW(dialog, IDC_TARGET_LUFS, EM_SETLIMITTEXT, 12, 0);
        Check(dialog, IDC_DITHER, s.dither); Check(dialog, IDC_INSTANT_BR, s.instant_bitrate); Check(dialog, IDC_UNIFIED_INFO, s.unified_info);
        SetDlgItemTextW(dialog, IDC_OUTPUT_STATUS, data->status.c_str()); LocalizeDialog(dialog); return TRUE;
    }
    if (message == WM_COMMAND && data) {
        const auto id = LOWORD(wparam);
        if ((id == IDC_OUTPUT_SOURCE || id == IDC_OUTPUT_STEREO) && HIWORD(wparam) == BN_CLICKED) {
            const auto mode = id == IDC_OUTPUT_SOURCE ? OutputMode::SourceChannels : OutputMode::Stereo;
            SendDlgItemMessageW(dialog, IDC_OUTPUT_MODE, CB_SETCURSEL, static_cast<int>(mode), 0); SyncRadios(dialog, mode); return TRUE;
        }
        if (id == IDC_OUTPUT_MODE && HIWORD(wparam) == CBN_SELCHANGE) {
            SyncRadios(dialog, static_cast<OutputMode>(Selection(dialog, IDC_OUTPUT_MODE))); return TRUE;
        }
        if (id == IDC_ADVANCED) {
            auto copy = data->settings;
            const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_ADVANCED), dialog,
                ModalGuard<AdvancedProc>, reinterpret_cast<LPARAM>(&copy));
            if (result == IDOK) data->settings = std::move(copy);
            else if (result == -1) Prompt(dialog, L"Could not open advanced decoder settings.", L"Winamp Opus Input", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (id == IDC_MORE_SETTINGS) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
            MoreData more{data->settings};
            const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_MORE), dialog,
                ModalGuard<MoreProc>, reinterpret_cast<LPARAM>(&more));
            if (result == IDOK) data->settings = std::move(more.draft);
            else if (result == -1) Prompt(dialog, L"Could not open additional settings.", L"Winamp Opus Input", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (id == IDOK) {
            auto s = data->settings;
            s.output_mode = static_cast<OutputMode>(Selection(dialog, IDC_OUTPUT_MODE));
            // Old automation may change a radio directly without BN_CLICKED.
            // A changed combo always wins, including Mono/Quad/DS/DS2 choices.
            if (s.output_mode == data->settings.output_mode) {
                if (Checked(dialog, IDC_OUTPUT_SOURCE)) s.output_mode = OutputMode::SourceChannels;
                else if (Checked(dialog, IDC_OUTPUT_STEREO)) s.output_mode = OutputMode::Stereo;
            }
            BOOL rate_ok = FALSE, lufs_ok = FALSE;
            s.sample_rate = static_cast<int>(GetDlgItemInt(dialog, IDC_PCM_RATE, &rate_ok, FALSE));
            s.target_lufs = static_cast<int>(GetDlgItemInt(dialog, IDC_TARGET_LUFS, &lufs_ok, TRUE));
            s.bits = Selection(dialog, IDC_PCM_BITS); s.resample_quality = Selection(dialog, IDC_RESAMPLE_QUALITY);
            s.gain_mode = static_cast<GainMode>(Selection(dialog, IDC_GAIN_MODE));
            if (static_cast<unsigned>(s.output_mode) > 6 || s.bits < 0 || s.resample_quality < 0 || static_cast<unsigned>(s.gain_mode) > 4 ||
                !rate_ok || (s.sample_rate != 0 && (s.sample_rate < 8000 || s.sample_rate > 192000)) || !lufs_ok ||
                s.target_lufs < -40 || s.target_lufs > 0 || !ReadDb(dialog, IDC_PRE_GAIN, s.pre_gain_q8) || !ReadDb(dialog, IDC_RADIO_GAIN, s.radio_gain_q8)) {
                Prompt(dialog, L"Use rate 0 (native) or 8,000 to 192,000 Hz, gains from -60 to +30 dB, and a target from -40 to 0 LUFS.",
                    L"Invalid audio settings", MB_OK | MB_ICONWARNING); return TRUE;
            }
            s.dither = Checked(dialog, IDC_DITHER); s.instant_bitrate = Checked(dialog, IDC_INSTANT_BR); s.unified_info = Checked(dialog, IDC_UNIFIED_INFO);
            if (!SavePlaybackSettings(s)) {
                Prompt(dialog, L"Could not save settings for the current user.", L"Winamp Opus Input", MB_OK | MB_ICONERROR); return TRUE;
            }
            data->settings = ValidateSettings(std::move(s)); EndDialog(dialog, IDOK); return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
}
PlaybackSettings ShowPlaybackConfigDialog(HINSTANCE instance, HWND parent, PlaybackSettings current, const std::wstring& playback_status) {
    ConfigData data{ValidateSettings(current), playback_status};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_CONFIG), parent, ModalGuard<ConfigProc>, reinterpret_cast<LPARAM>(&data));
    if (result == -1) Prompt(parent, L"Could not open the plugin settings dialog.", L"Winamp Opus Input", MB_OK | MB_ICONERROR);
    return result == IDOK ? data.settings : current;
}
OutputMode ShowOutputConfigDialog(HINSTANCE instance, HWND parent, OutputMode current, const std::wstring& status) {
    auto s = LoadPlaybackSettings(); s.output_mode = current;
    return ShowPlaybackConfigDialog(instance, parent, std::move(s), status).output_mode;
}
} // namespace winamp_opus
