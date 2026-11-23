#include "output_settings.hpp"
#include "language.hpp"
#include "registry_sandbox.hpp"
#include "resource.h"
#include <commctrl.h>
#include <array>
#include <climits>
#include <cstring>
#include <iostream>
#include <string>

using namespace winamp_opus;
namespace {
#define ALL_FIELDS(X) X(output_mode) X(sample_rate) X(bits) X(resample_quality) X(gain_mode) X(pre_gain_q8) \
    X(radio_gain_q8) X(target_lufs) X(dither) X(instant_bitrate) X(unified_info) X(opus_hd) X(opus_bwe) \
    X(mono_compatible) X(ignore_extensions) X(speech) X(ambisonic) X(recovery) X(recovery_limit_ms) \
    X(dither_pdf) X(noise_shape) X(custom_quantizer) X(use_host_replaygain) X(use_host_proxy) \
    X(preferred_gain_only) X(prevent_clipping) X(peak_mode) X(non_rg_q8) X(buffer_small_files) \
    X(timeout_ms) X(probe_kb) X(reconnect_attempts) X(reconnect_delay_ms) X(prebuffer_kb) X(stream_buffer_kb) \
    X(underrun_percent) X(save_streams) X(interactive_auth) X(http_seek) X(proxy_mode) X(proxy_url) X(save_directory)
void Equal(const PlaybackSettings& a, const PlaybackSettings& b) {
#define EQUAL_FIELD(name) Check(a.name == b.name, "settings mismatch: " #name);
    ALL_FIELDS(EQUAL_FIELD)
#undef EQUAL_FIELD
}
void Raw(const wchar_t* name, DWORD type, const void* value, DWORD bytes) {
    HKEY key = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "open sandbox key");
    const auto result = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(value), bytes);
    RegCloseKey(key); Check(result == ERROR_SUCCESS, "write sandbox value");
}
void Dword(const wchar_t* name, int value) {
    const DWORD stored = static_cast<DWORD>(value); Raw(name, REG_DWORD, &stored, sizeof(stored));
}
PlaybackSettings Nondefault() {
    PlaybackSettings s;
    s.output_mode = OutputMode::SurroundII; s.sample_rate = 0; s.bits = 32; s.resample_quality = 10;
    s.gain_mode = GainMode::Raw; s.pre_gain_q8 = -15360; s.radio_gain_q8 = 7680; s.target_lufs = -40;
    s.dither = false; s.instant_bitrate = false; s.unified_info = false; s.opus_hd = false; s.opus_bwe = true;
    s.mono_compatible = true; s.ignore_extensions = true; s.speech = SpeechMode::NoLace; s.ambisonic = AmbisonicMode::Surround71;
    s.recovery = RecoveryMode::Dred; s.recovery_limit_ms = 1000;
    s.dither_pdf = DitherPdf::Gaussian; s.noise_shape = NoiseShape::AthLow; s.custom_quantizer = true;
    s.use_host_replaygain = true; s.use_host_proxy = false; s.preferred_gain_only = true; s.prevent_clipping = true;
    s.peak_mode = 3; s.non_rg_q8 = -15360; s.buffer_small_files = true;
    s.timeout_ms = 30000; s.probe_kb = 4096; s.reconnect_attempts = 10; s.reconnect_delay_ms = 30000;
    s.stream_buffer_kb = 4096; s.prebuffer_kb = 2048; s.underrun_percent = 100;
    s.save_streams = true; s.interactive_auth = true; s.http_seek = true; s.proxy_mode = 1;
    s.proxy_url = L"http://[2001:db8::1]:8080/"; s.save_directory = L"C:\\Audio\\\u041c\u0443\u0437\u044b\u043a\u0430";
    return s;
}
void Persistence() {
    Equal(LoadPlaybackSettings(), PlaybackSettings{});
    Check(!LoadPlaybackSettings().custom_quantizer && !LoadPlaybackSettings().use_host_replaygain && LoadPlaybackSettings().use_host_proxy,
        "legacy fast path and independent host defaults");
    const auto all = Nondefault(); Check(SavePlaybackSettings(all), "save every field"); Equal(LoadPlaybackSettings(), all);
    for (int mode = 0; mode <= 6; ++mode) {
        auto s = all; s.output_mode = static_cast<OutputMode>(mode);
        Check(SavePlaybackSettings(s), "save output mode"); Equal(LoadPlaybackSettings(), s);
    }
    for (int pdf = 0; pdf < 3; ++pdf) for (int shape = 0; shape < 4; ++shape) {
        auto s = all; s.dither_pdf = static_cast<DitherPdf>(pdf); s.noise_shape = static_cast<NoiseShape>(shape);
        Check(SavePlaybackSettings(s), "save quantizer modes"); Equal(LoadPlaybackSettings(), s);
    }
    Check(SavePlaybackSettings(all), "restore sample");
    Check(SaveOutputMode(OutputMode::Mono), "compatibility save"); auto expected = all; expected.output_mode = OutputMode::Mono;
    Equal(LoadPlaybackSettings(), expected);
    Check(SavePlaybackSettings(PlaybackSettings{}), "restore default fields");
}
void Ranges() {
    auto s = Nondefault();
    s.output_mode = static_cast<OutputMode>(-1); s.dither_pdf = static_cast<DitherPdf>(3); s.noise_shape = static_cast<NoiseShape>(4);
    s.non_rg_q8 = INT_MIN; s.peak_mode = INT_MAX; s.timeout_ms = INT_MAX; s.probe_kb = INT_MIN;
    s.reconnect_attempts = INT_MAX; s.reconnect_delay_ms = INT_MIN; s.stream_buffer_kb = INT_MAX;
    s.prebuffer_kb = INT_MAX; s.underrun_percent = INT_MIN; s.proxy_mode = INT_MAX;
    const auto v = ValidateSettings(s);
    Check(v.output_mode == OutputMode::SourceChannels && v.dither_pdf == DitherPdf::Triangular && v.noise_shape == NoiseShape::None,
        "invalid enum defaults");
    Check(v.non_rg_q8 == -15360 && v.peak_mode == 3 && v.timeout_ms == 30000 && v.probe_kb == 64 &&
        v.reconnect_attempts == 10 && v.reconnect_delay_ms == 250 && v.stream_buffer_kb == 4096 && v.prebuffer_kb == 32 &&
        v.underrun_percent == 0 && v.proxy_mode == 2, "all added numeric bounds and dependency");
    Check(SavePlaybackSettings(s), "save invalid input safely"); Equal(LoadPlaybackSettings(), v);
    s = PlaybackSettings{}; s.non_rg_q8 = INT_MAX; s.peak_mode = INT_MIN; s.timeout_ms = INT_MIN; s.probe_kb = INT_MAX;
    s.reconnect_attempts = INT_MIN; s.reconnect_delay_ms = INT_MAX; s.stream_buffer_kb = INT_MIN;
    s.prebuffer_kb = INT_MIN; s.underrun_percent = INT_MAX; s.proxy_mode = INT_MIN;
    const auto w = ValidateSettings(s);
    Check(w.non_rg_q8 == 7680 && w.peak_mode == 0 && w.timeout_ms == 1000 && w.probe_kb == 4096 &&
        w.reconnect_attempts == 0 && w.reconnect_delay_ms == 30000 && w.stream_buffer_kb == 32 &&
        w.prebuffer_kb == 0 && w.underrun_percent == 100 && w.proxy_mode == 0, "opposite bounds");
    Check(SavePlaybackSettings(PlaybackSettings{}), "reset range test");
    Dword(L"DitherPdf", -1); Dword(L"NoiseShape", INT_MAX); Dword(L"HttpTimeoutMs", -1); Dword(L"ProbeLimitKb", INT_MAX);
    Dword(L"StreamBufferKb", -1); Dword(L"PrebufferKb", INT_MAX); Dword(L"NonReplayGainQ8", -999999); Dword(L"CustomQuantizer", 9);
    const auto raw = LoadPlaybackSettings();
    Check(raw.dither_pdf == DitherPdf::Triangular && raw.noise_shape == NoiseShape::None && raw.timeout_ms == 1000 &&
        raw.probe_kb == 4096 && raw.stream_buffer_kb == 32 && raw.prebuffer_kb == 32 && raw.non_rg_q8 == -15360 &&
        !raw.custom_quantizer, "corrupt redirected registry clamps and rejects malformed bool");
    const wchar_t wrong[] = L"30000"; Raw(L"HttpTimeoutMs", REG_SZ, wrong, sizeof(wrong));
    Check(LoadPlaybackSettings().timeout_ms == 5000, "wrong registry type exact default");
}
void Strings() {
    auto s = PlaybackSettings{};
    s.save_directory.assign(32767, L'\u0416'); Check(SavePlaybackSettings(s), "maximum Unicode path");
    Check(LoadPlaybackSettings().save_directory == s.save_directory, "path exact maximum round trip");
    s.save_directory.push_back(L'x'); Check(SavePlaybackSettings(s), "oversize path sanitized");
    Check(LoadPlaybackSettings().save_directory.empty(), "path length bound");
    for (const auto* proxy : {L"proxy.example:8080", L"http://proxy.example:8080/", L"[::1]:80", L""}) {
        s.proxy_url = proxy; Check(SavePlaybackSettings(s), "save credential-free proxy"); Check(LoadPlaybackSettings().proxy_url == proxy, "proxy string exact round trip");
    }
    for (const auto* proxy : {L"user:secret@proxy:80", L"http://user:secret@proxy/", L"proxy%40secret:80", L"proxy:80\r\nSecret: value",
        L"http=proxy:80;https=proxy:81", L"https://proxy:443", L"proxy:0", L"proxy:65536"}) {
        s.proxy_url = proxy; Check(SavePlaybackSettings(s), "sanitize unsafe proxy"); Check(LoadPlaybackSettings().proxy_url.empty(), "never store unsafe proxy credentials");
    }
    s.proxy_url.assign(2049, L'a'); s.save_directory = std::wstring(L"safe\0hidden", 11);
    Check(SavePlaybackSettings(s), "sanitize bounded strings"); Check(LoadPlaybackSettings().proxy_url.empty() && LoadPlaybackSettings().save_directory.empty(), "embedded NUL and oversize rejected");
    const wchar_t embedded[]{L'p',0,L'h',0}; Raw(L"ProxyUrl", REG_SZ, embedded, sizeof(embedded));
    Check(LoadPlaybackSettings().proxy_url.empty(), "raw embedded NUL rejected");
    const wchar_t no_null[]{L'p',L'r',L'o',L'x',L'y'}; Raw(L"ProxyUrl", REG_SZ, no_null, sizeof(no_null));
    Check(LoadPlaybackSettings().proxy_url.empty(), "unterminated registry string rejected");
    const BYTE odd[]{'a',0,0}; Raw(L"ProxyUrl", REG_SZ, odd, sizeof(odd));
    Check(LoadPlaybackSettings().proxy_url.empty(), "odd UTF-16 byte length rejected");
    const auto huge = std::wstring(65536, L'a'); Raw(L"SaveDirectory", REG_SZ, huge.c_str(), static_cast<DWORD>((huge.size()+1)*sizeof(wchar_t)));
    Check(LoadPlaybackSettings().save_directory.empty(), "registry length bounded before allocation");
}

HHOOK hook = nullptr;
int outer_result = IDOK, child_result = IDOK;
bool outer_seen = false, child_seen = false, check_invalid = false, warning_seen = false, legacy_advanced = false;
std::string failure;
HWND Page(HWND dialog, int id) {
    for (auto child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) if (GetDlgItem(child, id)) return child;
    return nullptr;
}
void Bounds(HWND dialog) {
    RECT client{}; GetClientRect(dialog, &client);
    for (auto child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT r{}; GetWindowRect(child, &r); MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&r), 2);
        Check(r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom, "native controls within client bounds");
    }
}
void CALLBACK CloseWarning(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer); warning_seen = true; PostMessageW(dialog, WM_CLOSE, 0, 0);
}
void CALLBACK InspectChild(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        Bounds(dialog);
        const auto process = Page(dialog, IDC_CUSTOM_QUANTIZER), gain = Page(dialog, IDC_HOST_REPLAYGAIN), network = Page(dialog, IDC_TIMEOUT_MS);
        Check(process && gain && network, "three native child pages"); Bounds(process); Bounds(gain); Bounds(network);
        Check((GetWindowLongPtrW(process, GWL_EXSTYLE) & WS_EX_CONTROLPARENT) != 0, "page keyboard navigation");
        Check(SendDlgItemMessageW(process, IDC_DITHER_PDF, CB_GETCOUNT, 0, 0) == 3 &&
            SendDlgItemMessageW(process, IDC_NOISE_SHAPE, CB_GETCOUNT, 0, 0) == 4, "all processing choices");
        Check(SendDlgItemMessageW(gain, IDC_PEAK_MODE, CB_GETCOUNT, 0, 0) == 4, "all gain modes");
        Check(SendDlgItemMessageW(network, IDC_PROXY_URL, EM_GETLIMITTEXT, 0, 0) == 2048 &&
            SendDlgItemMessageW(network, IDC_SAVE_DIRECTORY, EM_GETLIMITTEXT, 0, 0) == 32767, "UI text length bounds");
        CheckDlgButton(process, IDC_CUSTOM_QUANTIZER, BST_CHECKED);
        SendMessageW(process, WM_COMMAND, MAKEWPARAM(IDC_CUSTOM_QUANTIZER, BN_CLICKED), 0);
        Check(IsWindowEnabled(GetDlgItem(process, IDC_DITHER_PDF)) != FALSE, "custom quantizer enables controls");
        SendDlgItemMessageW(process, IDC_DITHER_PDF, CB_SETCURSEL, 2, 0); SendDlgItemMessageW(process, IDC_NOISE_SHAPE, CB_SETCURSEL, 3, 0);
        CheckDlgButton(process, IDC_BUFFER_SMALL_FILES, BST_CHECKED); CheckDlgButton(gain, IDC_HOST_REPLAYGAIN, BST_CHECKED);
        CheckDlgButton(gain, IDC_PREFERRED_GAIN_ONLY, BST_CHECKED); CheckDlgButton(gain, IDC_PREVENT_CLIPPING, BST_CHECKED);
        SendDlgItemMessageW(gain, IDC_PEAK_MODE, CB_SETCURSEL, 2, 0); SetDlgItemTextW(gain, IDC_NON_RG_GAIN, L"-1.125");
        CheckDlgButton(network, IDC_HOST_PROXY, BST_UNCHECKED); SetDlgItemInt(network, IDC_TIMEOUT_MS, 12345, FALSE);
        SetDlgItemInt(network, IDC_PROBE_KB, 512, FALSE); SetDlgItemInt(network, IDC_RECONNECT_ATTEMPTS, 2, FALSE);
        SetDlgItemInt(network, IDC_RECONNECT_DELAY, 750, FALSE); SetDlgItemInt(network, IDC_STREAM_BUFFER, 256, FALSE);
        SetDlgItemInt(network, IDC_PREBUFFER_KB, 128, FALSE); SetDlgItemInt(network, IDC_UNDERRUN_PERCENT, 30, FALSE);
        SendDlgItemMessageW(network, IDC_PROXY_MODE, CB_SETCURSEL, 1, 0); SetDlgItemTextW(network, IDC_PROXY_URL, L"proxy.example:8080");
        SetDlgItemTextW(network, IDC_SAVE_DIRECTORY, L"C:\\recordings");
        for (int id : {IDC_SAVE_STREAMS, IDC_INTERACTIVE_AUTH, IDC_HTTP_SEEK}) CheckDlgButton(network, id, BST_CHECKED);
        const auto tabs = GetDlgItem(dialog, IDC_SETTINGS_TABS);
        Check(TabCtrl_GetItemCount(tabs) == 3, "tab count");
        for (int index = 0; index < 3; ++index) {
            TabCtrl_SetCurSel(tabs, index); NMHDR notification{tabs, IDC_SETTINGS_TABS, TCN_SELCHANGE};
            SendMessageW(dialog, WM_NOTIFY, IDC_SETTINGS_TABS, reinterpret_cast<LPARAM>(&notification));
        }
        if (check_invalid) {
            SetDlgItemInt(network, IDC_PREBUFFER_KB, 257, FALSE);
            SendMessageW(dialog, WM_COMMAND, IDOK, 0);
            Check(IsWindow(dialog) && warning_seen, "invalid network input keeps modal transaction open");
            SetDlgItemInt(network, IDC_PREBUFFER_KB, 128, FALSE);
        }
        child_seen = true;
    } catch (const std::exception& e) { failure = e.what(); }
    PostMessageW(dialog, WM_COMMAND, failure.empty() ? child_result : IDCANCEL, 0);
}
void CALLBACK InspectAdvanced(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        Bounds(dialog);
        Check(SendDlgItemMessageW(dialog, IDC_OPUS_SPEECH, CB_GETCOUNT, 0, 0) == 3 &&
            SendDlgItemMessageW(dialog, IDC_AMBISONIC_MODE, CB_GETCOUNT, 0, 0) == 5 &&
            SendDlgItemMessageW(dialog, IDC_RECOVERY_MODE, CB_GETCOUNT, 0, 0) == 5, "legacy Advanced choices unchanged");
        CheckDlgButton(dialog, IDC_OPUS_HD, BST_UNCHECKED);
        for (int id : {IDC_OPUS_BWE, IDC_MONO_COMPATIBLE, IDC_IGNORE_EXTENSIONS}) CheckDlgButton(dialog, id, BST_CHECKED);
        SendDlgItemMessageW(dialog, IDC_OPUS_SPEECH, CB_SETCURSEL, 1, 0);
        SendDlgItemMessageW(dialog, IDC_AMBISONIC_MODE, CB_SETCURSEL, 4, 0);
        SendDlgItemMessageW(dialog, IDC_RECOVERY_MODE, CB_SETCURSEL, 4, 0);
        SetDlgItemInt(dialog, IDC_RECOVERY_LIMIT, 80, FALSE);
        child_seen = true;
    } catch (const std::exception& e) { failure = e.what(); }
    PostMessageW(dialog, WM_COMMAND, failure.empty() ? child_result : IDCANCEL, 0);
}
void CALLBACK InspectOuter(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        Bounds(dialog);
        Check(SendDlgItemMessageW(dialog, IDC_OUTPUT_MODE, CB_GETCOUNT, 0, 0) == 7, "all channel modes available");
        // No CBN_SELCHANGE on purpose: apply must not overwrite this with the old radio.
        SendDlgItemMessageW(dialog, IDC_OUTPUT_MODE, CB_SETCURSEL, 6, 0);
        SendMessageW(dialog, WM_COMMAND, legacy_advanced ? IDC_ADVANCED : IDC_MORE_SETTINGS, 0);
        Check(child_seen, "inspected modal child"); outer_seen = true;
    } catch (const std::exception& e) { failure = e.what(); }
    PostMessageW(dialog, WM_COMMAND, failure.empty() ? outer_result : IDCANCEL, 0);
}
LRESULT CALLBACK Hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message.message == WM_INITDIALOG) {
            if (GetDlgItem(message.hwnd, IDC_OPUS_HD)) SetTimer(message.hwnd, 94, 10, InspectAdvanced);
            else if (GetDlgItem(message.hwnd, IDC_SETTINGS_TABS)) SetTimer(message.hwnd, 92, 10, InspectChild);
            else if (GetDlgItem(message.hwnd, IDC_MORE_SETTINGS)) SetTimer(message.hwnd, 91, 10, InspectOuter);
            else if (GetDlgItem(message.hwnd, 0xffff) && GetDlgItem(message.hwnd, 20)) SetTimer(message.hwnd, 93, 10, CloseWarning);
        }
    }
    return CallNextHookEx(hook, code, wparam, lparam);
}
PlaybackSettings Show(int outer, int child, bool invalid = false, bool advanced = false) {
    outer_result = outer; child_result = child; check_invalid = invalid; legacy_advanced = advanced;
    outer_seen = child_seen = warning_seen = false; failure.clear();
    hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Hook, nullptr, GetCurrentThreadId()); Check(hook != nullptr, "dialog test hook");
    auto result = ShowPlaybackConfigDialog(GetModuleHandleW(nullptr), nullptr, PlaybackSettings{}, L"Testing");
    UnhookWindowsHookEx(hook); hook = nullptr;
    Check(failure.empty(), failure.c_str()); Check(outer_seen && child_seen, "real settings dialogs executed");
    return result;
}
void Dialogs() {
    Check(SavePlaybackSettings(PlaybackSettings{}), "default dialog baseline");
    Equal(Show(IDCANCEL, IDOK), PlaybackSettings{}); Equal(LoadPlaybackSettings(), PlaybackSettings{});
    auto child_cancel = Show(IDOK, IDCANCEL); auto expected = PlaybackSettings{}; expected.output_mode = OutputMode::SurroundII;
    Equal(child_cancel, expected); Equal(LoadPlaybackSettings(), expected);
    Check(SavePlaybackSettings(PlaybackSettings{}), "reset dialog baseline");
    const auto saved = Show(IDOK, IDOK, true);
    Check(saved.output_mode == OutputMode::SurroundII && saved.custom_quantizer && saved.dither_pdf == DitherPdf::Gaussian &&
        saved.noise_shape == NoiseShape::AthLow && saved.buffer_small_files && saved.use_host_replaygain && !saved.use_host_proxy &&
        saved.preferred_gain_only && saved.prevent_clipping && saved.peak_mode == 2 && saved.non_rg_q8 == -288 && saved.timeout_ms == 12345 &&
        saved.probe_kb == 512 && saved.reconnect_attempts == 2 && saved.reconnect_delay_ms == 750 && saved.stream_buffer_kb == 256 &&
        saved.prebuffer_kb == 128 && saved.underrun_percent == 30 && saved.proxy_mode == 1 && saved.proxy_url == L"proxy.example:8080" &&
        saved.save_directory == L"C:\\recordings" && saved.save_streams && saved.interactive_auth && saved.http_seek, "every new UI field accepted");
    Equal(LoadPlaybackSettings(), saved);
    Check(SavePlaybackSettings(PlaybackSettings{}), "legacy Advanced default baseline");
    Equal(Show(IDCANCEL, IDOK, false, true), PlaybackSettings{}); Equal(LoadPlaybackSettings(), PlaybackSettings{});
    Equal(Show(IDOK, IDCANCEL, false, true), expected); Equal(LoadPlaybackSettings(), expected);
    const auto advanced = Show(IDOK, IDOK, false, true);
    expected.opus_hd = false; expected.opus_bwe = expected.mono_compatible = expected.ignore_extensions = true;
    expected.speech = SpeechMode::Lace; expected.ambisonic = AmbisonicMode::Surround71;
    expected.recovery = RecoveryMode::Dred; expected.recovery_limit_ms = 80;
    Equal(advanced, expected); Equal(LoadPlaybackSettings(), expected);
}
}
int main() {
    try {
        RegistrySandbox sandbox;
        Persistence(); Ranges(); Strings(); Dialogs();
        std::cout << "Settings features: all fields persisted, ranges and bounded strings, native tabs, validation, parent/child cancellation passed.\n";
        return 0;
    } catch (const std::exception& e) {
        if (hook) UnhookWindowsHookEx(hook);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
