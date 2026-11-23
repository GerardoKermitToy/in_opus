#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "dialog_capture.hpp"
#include "output_settings.hpp"
#include "resource.h"
#include "winamp_input.hpp"
#include <array>
#include <iostream>
#include <string>

using namespace winamp_opus;
namespace {
HHOOK hook = nullptr;
int outer_result = IDOK, inner_result = IDOK;
bool outer_seen = false, inner_seen = false;
std::string failure;
std::filesystem::path directory;
void Bounds(HWND dialog) {
    RECT client{}; GetClientRect(dialog, &client);
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{}; GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        Check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
            "advanced settings control clipped");
    }
}
void CALLBACK InspectInner(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        Bounds(dialog);
        for (int id : {IDC_OPUS_HD, IDC_OPUS_BWE, IDC_MONO_COMPATIBLE, IDC_IGNORE_EXTENSIONS})
            Check(GetDlgItem(dialog, id) != nullptr, "advanced checkbox exists");
        Check(SendDlgItemMessageW(dialog, IDC_OPUS_SPEECH, CB_GETCOUNT, 0, 0) == 3, "speech presets");
        Check(SendDlgItemMessageW(dialog, IDC_AMBISONIC_MODE, CB_GETCOUNT, 0, 0) == 5, "ambisonic targets");
        Check(SendDlgItemMessageW(dialog, IDC_RECOVERY_MODE, CB_GETCOUNT, 0, 0) == 5, "recovery modes");
        if (inner_result == IDOK) {
            CheckDlgButton(dialog, IDC_OPUS_HD, BST_UNCHECKED);
            for (int id : {IDC_OPUS_BWE, IDC_MONO_COMPATIBLE, IDC_IGNORE_EXTENSIONS}) CheckDlgButton(dialog, id, BST_CHECKED);
            SendDlgItemMessageW(dialog, IDC_OPUS_SPEECH, CB_SETCURSEL, 1, 0);
            SendDlgItemMessageW(dialog, IDC_AMBISONIC_MODE, CB_SETCURSEL, 4, 0);
            SendDlgItemMessageW(dialog, IDC_RECOVERY_MODE, CB_SETCURSEL, 4, 0);
            SetDlgItemInt(dialog, IDC_RECOVERY_LIMIT, 80, FALSE);
        }
        CaptureDialog(dialog, (directory / L"advanced.bmp").c_str()); inner_seen = true;
    } catch (const std::exception& e) { failure = e.what(); }
    PostMessageW(dialog, WM_COMMAND, failure.empty() ? inner_result : IDCANCEL, 0);
}
void CALLBACK InspectOuter(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        Bounds(dialog);
        SetDlgItemInt(dialog, IDC_PCM_RATE, 0, FALSE);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        CaptureDialog(dialog, (directory / L"config.bmp").c_str());
        SendMessageW(dialog, WM_COMMAND, IDC_ADVANCED, 0);
        Check(inner_seen, "advanced child was inspected");
        outer_seen = true;
    } catch (const std::exception& e) { failure = e.what(); }
    PostMessageW(dialog, WM_COMMAND, failure.empty() ? outer_result : IDCANCEL, 0);
}
LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(l);
        if (message.message == WM_INITDIALOG) {
            if (GetDlgItem(message.hwnd, IDC_OPUS_HD)) SetTimer(message.hwnd, 92, 20, InspectInner);
            else if (GetDlgItem(message.hwnd, IDC_ADVANCED)) SetTimer(message.hwnd, 91, 20, InspectOuter);
        }
    }
    return CallNextHookEx(hook, code, w, l);
}
void Show(winamp::InModule* input, int outer, int inner) {
    outer_result = outer; inner_result = inner; outer_seen = inner_seen = false; failure.clear();
    hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Hook, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "advanced dialog hook");
    input->Config(nullptr);
    UnhookWindowsHookEx(hook); hook = nullptr;
    Check(failure.empty(), failure.c_str()); Check(outer_seen && inner_seen, "settings dialogs shown");
}
}
int wmain(int argc, wchar_t** argv) {
    HMODULE dll = nullptr;
    try {
        RegistrySandbox sandbox;
        Check(argc == 3, "usage: advanced_settings_tests DLL DATA_DIR");
        directory = argv[2]; std::filesystem::create_directories(directory);
        const auto defaults = LoadPlaybackSettings();
        Check(defaults.opus_hd && !defaults.opus_bwe && defaults.speech == SpeechMode::Off &&
            !defaults.mono_compatible && !defaults.ignore_extensions && defaults.recovery == RecoveryMode::Strict,
            "backwards-compatible defaults");
        PlaybackSettings invalid;
        invalid.sample_rate = 0; invalid.speech = static_cast<SpeechMode>(999);
        invalid.ambisonic = static_cast<AmbisonicMode>(999); invalid.recovery = static_cast<RecoveryMode>(999);
        invalid.recovery_limit_ms = -1;
        invalid = ValidateSettings(invalid);
        Check(invalid.sample_rate == 0 && invalid.speech == SpeechMode::Off && invalid.ambisonic == AmbisonicMode::Stereo &&
            invalid.recovery == RecoveryMode::Strict && invalid.recovery_limit_ms == 20, "validate new settings");
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll != nullptr, "load settings DLL");
        const auto get = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        Check(get != nullptr, "settings input export");
        auto* input = get(); input->hDllInstance = dll; input->Init();
        Show(input, IDCANCEL, IDOK);
        auto saved = LoadPlaybackSettings();
        Check(saved.opus_hd && !saved.opus_bwe && saved.sample_rate == 48000 && saved.recovery == RecoveryMode::Strict,
            "outer Cancel must not persist accepted child settings");
        Show(input, IDOK, IDCANCEL);
        saved = LoadPlaybackSettings();
        Check(saved.sample_rate == 0 && saved.opus_hd && !saved.opus_bwe && saved.recovery == RecoveryMode::Strict,
            "child Cancel preserves advanced settings");
        Show(input, IDOK, IDOK);
        saved = LoadPlaybackSettings();
        Check(saved.sample_rate == 0 && !saved.opus_hd && saved.opus_bwe && saved.mono_compatible && saved.ignore_extensions &&
            saved.speech == SpeechMode::Lace && saved.ambisonic == AmbisonicMode::Surround71 &&
            saved.recovery == RecoveryMode::Dred && saved.recovery_limit_ms == 80, "advanced OK round trip");
        for (auto speech : {SpeechMode::Off, SpeechMode::Lace, SpeechMode::NoLace})
            for (int recovery = 0; recovery < 5; ++recovery) {
                saved.speech = speech; saved.recovery = static_cast<RecoveryMode>(recovery);
                Check(SavePlaybackSettings(saved), "save each advanced preset");
                const auto actual = LoadPlaybackSettings();
                Check(actual.speech == speech && actual.recovery == saved.recovery, "load each advanced preset");
            }
        input->Quit(); FreeLibrary(dll); dll = nullptr;
        std::cout << "Advanced settings: validation, defaults, DLL dialogs, bounds, nested OK/Cancel and persistence passed.\n";
        return 0;
    } catch (const std::exception& e) {
        if (hook) UnhookWindowsHookEx(hook);
        if (dll) FreeLibrary(dll);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
