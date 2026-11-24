#include "fixtures.hpp"
#include "dialog_capture.hpp"
#include "registry_sandbox.hpp"
#include "opus_decoder.hpp"
#include "resource.h"
#include "winamp_input.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
HHOOK hook = nullptr;
const winamp_opus::TrackInfo* expected = nullptr;
const wchar_t* expected_path = nullptr;
const wchar_t* screenshot_path = nullptr;
std::wstring expected_duration;
std::string failure;
int inspected = 0;
int close_command = IDOK;

std::wstring Text(HWND window, int id) {
    const HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "missing dialog control");
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int length = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(length);
    return text;
}
void CALLBACK Inspect(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        wchar_t caption[128]{};
        GetWindowTextW(dialog, caption, 128);
        Check(std::wstring(caption) == L"Opus File Information", "English file information caption");
        Check(Text(dialog, IDOK) == L"&Close", "English Close button");
        Check(Text(dialog, IDC_INFO_TITLE) == (expected->title.empty() ? expected->display_title : expected->title), "dialog title metadata");
        Check(Text(dialog, IDC_INFO_DURATION) == expected_duration, "dialog duration formatting");
        Check(Text(dialog, IDC_INFO_BITRATE) == std::to_wstring((static_cast<long long>(expected->bitrate) + 500) / 1000) + L" kbps", "rounded bitrate");
        Check(Text(dialog, IDC_INFO_ARTIST) == (expected->artist.empty() ? L"—" : expected->artist), "artist fallback");
        Check(Text(dialog, IDC_INFO_ALBUM) == (expected->album.empty() ? L"—" : expected->album), "album fallback");
        Check(Text(dialog, IDC_INFO_OUTPUT) == L"48,000 Hz, 16-bit, stereo", "PCM description");
        Check(Text(dialog, IDC_INFO_PATH) == expected_path, "Unicode path display");
        Check((GetWindowLongPtrW(GetDlgItem(dialog, IDC_INFO_PATH), GWL_STYLE) & ES_READONLY) != 0, "read-only path");
        RECT client{};
        GetClientRect(dialog, &client);
        LONG value_left = -1;
        for (int id : {IDC_INFO_FORMAT, IDC_INFO_DURATION, IDC_INFO_BITRATE, IDC_INFO_CHANNELS, IDC_INFO_OUTPUT, IDOK}) {
            RECT control{};
            GetWindowRect(GetDlgItem(dialog, id), &control);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&control), 2);
            Check(control.left >= 0 && control.top >= 0 && control.right <= client.right && control.bottom <= client.bottom, "control clipped by dialog");
            if (id != IDOK) {
                if (value_left < 0) value_left = control.left;
                Check(control.left == value_left, "property values not aligned");
            }
        }
        if (screenshot_path && inspected == 0) CaptureDialog(dialog, screenshot_path);
        ++inspected;
    } catch (const std::exception& error) {
        failure = error.what();
    } catch (...) {
        failure = "unexpected dialog test failure";
    }
    PostMessageW(dialog, WM_COMMAND, close_command, 0);
}
LRESULT CALLBACK OnDialog(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message.message == WM_INITDIALOG) SetTimer(message.hwnd, 42, 100, Inspect);
    }
    return CallNextHookEx(hook, code, wparam, lparam);
}
std::wstring ExpectedDuration(int ms) {
    std::wostringstream value;
    const int seconds = ms / 1000;
    if (seconds >= 3600) value << seconds / 3600 << L':' << std::setw(2) << std::setfill(L'0') << seconds / 60 % 60;
    else value << seconds / 60;
    value << L':' << std::setw(2) << std::setfill(L'0') << seconds % 60 << L'.' << std::setw(3) << ms % 1000;
    return value.str();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    registry.StoreOutputMode(static_cast<DWORD>(winamp_opus::OutputMode::Stereo));
    HMODULE library = nullptr;
    try {
        Check(argc >= 3 && argc <= 5, "usage: dialog_tests DLL directory [audio-file [screenshot.bmp]]");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        auto path = directory / L"Свойства файла.opus";
        if (argc >= 4) path = argv[3];
        else MakeOpus(path, 2, 144117, 301);
        winamp_opus::Decoder decoder;
        Check(decoder.Open(path.c_str()), "open dialog fixture");
        expected = &decoder.Info();
        expected_path = path.c_str();
        expected_duration = ExpectedDuration(expected->length_ms);
        if (argc == 5) screenshot_path = argv[4];
        library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(library != nullptr, "load dialog DLL");
        const auto get_module = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(library, "winampGetInModule2"));
        Check(get_module != nullptr, "input module export");
        auto* module = get_module();
        module->hDllInstance = library;
        module->Init();
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, OnDialog, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install own-thread dialog hook");
        Check(module->InfoBox(path.c_str(), nullptr) == 1, "read-only InfoBox return value");
        close_command = IDCANCEL;
        Check(module->InfoBox(path.c_str(), nullptr) == 1, "cancel InfoBox return value");
        UnhookWindowsHookEx(hook); hook = nullptr;
        Check(failure.empty(), failure.c_str());
        Check(inspected == 2, "native property dialog not inspected");
        using Unified = int (__cdecl*)(const wchar_t*);
        using Pane = HWND (__cdecl*)(int, const wchar_t*, HWND, wchar_t*, int);
        const auto unified = reinterpret_cast<Unified>(GetProcAddress(library, "winampUseUnifiedFileInfoDlg"));
        const auto pane_export = reinterpret_cast<Pane>(GetProcAddress(library, "winampAddUnifiedFileInfoPane"));
        Check(unified && pane_export, "unified information exports");
        Check(unified(path.c_str()) == 1 && unified(nullptr) == 0 &&
            unified(L"https://example.invalid/radio.opus") == 0, "local-only unified dialog routing");
        struct Window { HWND value = nullptr; ~Window() { if (value) DestroyWindow(value); } } parent;
        parent.value = CreateWindowExW(0, L"STATIC", L"Unified information test", WS_POPUP, 0, 0, 600, 400,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(parent.value != nullptr, "create unified test owner");
        const auto edit = CreateWindowExW(0, L"EDIT", L"Metadata", WS_CHILD, 0, 0, 100, 20, parent.value,
            nullptr, GetModuleHandleW(nullptr), nullptr);
        const auto combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWN, 0, 24, 100, 100,
            parent.value, nullptr, GetModuleHandleW(nullptr), nullptr);
        wchar_t name[100]{};
        const auto pane = pane_export(0, path.c_str(), parent.value, name, 100);
        Check(pane && GetPropW(parent.value, L"INBUILT_NOWRITEINFO"), "Advanced owns the unified commit transaction");
        Check(std::wstring(name) == L"Advanced" && GetDlgItem(pane, IDC_TAG_LIST), "one native Advanced editor");
        Check(!GetDlgItem(pane, IDC_INFO_TAGS) && !GetDlgItem(pane, IDC_INFO_PATH) &&
            !GetDlgItem(pane, IDC_EDIT_TAGS) && !GetDlgItem(pane, IDOK) && !GetDlgItem(pane, IDCANCEL),
            "Advanced does not duplicate host technical fields, path, launchers or OK/Cancel");
        Check((GetWindowLongPtrW(GetDlgItem(pane, IDC_TAG_VALUE), GWL_STYLE) & ES_MULTILINE) != 0,
            "Advanced uses a multiline value editor");
        Check((GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) == 0 && IsWindowEnabled(combo),
            "unified pane must not freeze sibling host controls");
        Check(pane_export(1, path.c_str(), parent.value, name, 100) == nullptr, "one unified Opus pane");
        DestroyWindow(parent.value); parent.value = nullptr;
        module->Quit();
        FreeLibrary(library); library = nullptr;
        std::cout << "Native dialog passed: metadata, aligned fields, duration, bitrate, Unicode path, Close/Cancel.\n";
        return 0;
    } catch (const std::exception& error) {
        if (hook) UnhookWindowsHookEx(hook);
        if (library) FreeLibrary(library);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
