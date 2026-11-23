#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "output_settings.hpp"
#include "opus_decoder.hpp"
#include "winamp_input.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace winamp_opus;
namespace {
struct InfoUpdate { int bitrate, rate, channels, sync; };
std::vector<InfoUpdate> updates;
std::atomic<int> errors{0}, eof{0};
std::atomic<std::int64_t> written{0};
std::atomic<int> flushes{0};
struct SeekRequest { int write; std::vector<int> positions; };
std::vector<SeekRequest> seeks;
std::atomic<int> writes{0}, canwrite_calls{0}, playing_calls{0};
int opens = 0, pause_at = -1;
int title_updates = 0;
std::wstring last_title;
winamp::InModule* input = nullptr;

void Verify(bool value) { if (!value) ++errors; }
int __cdecl Open(int rate, int channels, int bits, int, int) {
    Verify(rate == 48000 && channels == 2 && bits == 16); ++opens; return 0;
}
void __cdecl Close() {}
int __cdecl Write(char* data, int bytes) {
    Verify(data && bytes > 0 && bytes <= 8192 && bytes % 4 == 0);
    written += bytes / 4;
    // Exercise several wall-clock bitrate ticks instead of draining the entire
    // fixture before the first 250-ms update. No real output device is used.
    Sleep(8);
    ++writes;
    if (writes == pause_at) input->Pause();
    for (const auto& seek : seeks) if (seek.write == writes)
        for (int position : seek.positions) input->SetOutputTime(position);
    return 0;
}
int __cdecl CanWrite() { ++canwrite_calls; return 8192; }
int __cdecl Playing() { ++playing_calls; return 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl Flush(int) { ++flushes; }
int __cdecl Time() { return 0; }
void __cdecl SetInfo(int bitrate, int rate, int channels, int sync) {
    Verify(bitrate > 0 && sync == 1);
    Verify((rate == 48 && channels == 2) || (rate == -1 && channels == -1));
    updates.push_back({bitrate, rate, channels, sync});
}
LRESULT CALLBACK Host(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == winamp::kEndOfFile) { ++eof; return 0; }
    if (message == WM_USER && l == 243) {
        ++title_updates;
        wchar_t title[winamp::kTitleCapacity]{};
        input->GetFileInfo(nullptr, title, nullptr);
        last_title = title;
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Run(const std::filesystem::path& path, bool native, bool instantaneous, bool chained) {
    PlaybackSettings settings;
    settings.output_mode = OutputMode::Stereo;
    settings.mono_compatible = native; // Force the advanced backend without changing the output format.
    settings.instant_bitrate = instantaneous;
    Check(SavePlaybackSettings(settings), "save notification-test settings");
    input->Init();
    Decoder reference;
    Check(reference.Open(path.c_str(), nullptr, settings), "open notification reference");
    Check(reference.NativeActive() == native, "exercise the intended decoder backend");
    const auto frames = reference.Info().total_frames;
    reference.Close();

    Pump(); updates.clear(); seeks.clear(); writes = opens = 0; flushes = 0;
    errors = eof = 0; written = 0; title_updates = 0; last_title.clear();
    const auto started = GetTickCount64();
    Check(input->Play(path.c_str()) == 0, "notification-test playback");
    while (!eof && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    Pump();
    input->Stop(); // Joins the worker before inspecting its callback records.
    Pump();
    Check(eof == 1 && written == frames, "notification-test exact playback and EOF");
    Check(GetTickCount64() - started >= 1000, "exercise multiple periodic bitrate intervals");
    Check(errors == 0, "notification callback ABI contract");
    Check(!updates.empty() && updates.front().rate == 48 && updates.front().channels == 2,
        "initial SetInfo must publish the complete output format");
    int full_format_updates = 0;
    for (const auto& update : updates) if (update.rate != -1) ++full_format_updates;
    Check(full_format_updates == 1,
        "periodic bitrate updates must not resend sample rate/channels and trigger playlist title redraws");
    for (std::size_t i = 1; i < updates.size(); ++i) {
        Check(updates[i].rate == -1 && updates[i].channels == -1,
            "periodic SetInfo must preserve the established format");
        Check(updates[i].bitrate != updates[i - 1].bitrate,
            "unchanged displayed bitrate must not emit another SetInfo notification");
    }
    if (!instantaneous) Check(updates.size() == 1, "average-bitrate mode must not emit periodic SetInfo");
    else if (chained) {
        Check(updates.size() >= 3, "changed instantaneous bitrates must still reach the host");
        Check(updates.back().bitrate > updates[1].bitrate,
            "the higher-bitrate next link must update the displayed bitrate");
    } else {
        Check(updates.size() <= 2, "CBR track must not produce repeated identical notifications");
    }
    Check(title_updates == (chained ? 1 : 0), "only a real logical-link change requests a title update");
    if (chained) Check(last_title.find(L"Second track") != std::wstring::npos,
        "real chained metadata changes must still update the playlist title");
    std::cout << (native ? "Native" : "libopusfile") << (chained ? " chained" : " CBR")
        << (instantaneous ? " instant" : " average") << ": SetInfo=" << updates.size()
        << ", full-format=" << full_format_updates << ", title=" << title_updates
        << ", final-kbps=" << updates.back().bitrate << '\n';
}
void WaitUntil(const std::function<bool()>& ready, const char* message) {
    const auto started = GetTickCount64();
    while (!ready() && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    Check(ready(), message);
}
void RunIdle(const std::filesystem::path& path) {
    PlaybackSettings settings; settings.instant_bitrate = false;
    Check(SavePlaybackSettings(settings), "save idle notification settings"); input->Init();
    Pump(); updates.clear(); seeks.clear(); errors = eof = 0; written = 0;
    title_updates = 0; writes = opens = 0; flushes = 0; pause_at = 4;
    Check(input->Play(path.c_str()) == 0, "play idle fixture");
    WaitUntil([] { return input->IsPaused() != 0; }, "pause enters event wait");
    const int before = writes;
    input->SetOutputTime(100);
    WaitUntil([] { return flushes.load() == 1; }, "seek wakes paused worker");
    Sleep(80); Pump();
    Check(writes == before && title_updates == 0, "paused seek neither decodes nor redraws playlist");
    input->UnPause();
    WaitUntil([] { return eof.load() == 1; }, "resume wakes worker to EOF");
    Sleep(30); const int queries = canwrite_calls, playing = playing_calls;
    Sleep(80); Pump();
    Check(canwrite_calls == queries && playing_calls == playing, "EOF stops polling the output plugin");
    input->SetOutputTime(1800);
    WaitUntil([] { return eof.load() == 2; }, "seek restarts playback after EOF");
    const auto stopped = GetTickCount64(); input->Stop(); Pump();
    Check(GetTickCount64() - stopped < 1000, "stop wakes an idle worker");
    Check(opens == 1 && updates.size() == 1 && title_updates == 0 && errors == 0,
        "idle control changes do not republish format or playlist metadata");
    pause_at = -1;
}
void RunSeeks(const std::filesystem::path& path, bool native, int expected_titles) {
    PlaybackSettings settings;
    settings.mono_compatible = native;
    settings.instant_bitrate = false;
    Check(SavePlaybackSettings(settings), "save seek notification settings");
    input->Init();
    Pump(); updates.clear(); errors = eof = 0; written = 0; title_updates = 0;
    last_title.clear(); writes = opens = 0; flushes = 0;
    // The burst must be coalesced to its final request. The next seek stays
    // within that link; the last returns to the first link.
    seeks = {{4, {300, 500, 1400}}, {8, {1600}}, {12, {100}}};
    Check(input->Play(path.c_str()) == 0, "play repeated seek fixture");
    const auto started = GetTickCount64();
    while (!eof && GetTickCount64() - started < 15000) { Pump(); Sleep(1); }
    input->Stop(); Pump();
    Check(eof == 1 && errors == 0, "seek playback completes without ABI errors");
    Check(flushes == 3 && opens == 1, "seek burst coalesces without output reopening");
    Check(updates.size() == 1, "seek does not resend the output format or status");
    std::cout << (native ? "Native" : "libopusfile") << " repeated seeks: title=" << title_updates << '\n';
    Check(title_updates == expected_titles, "seek and unchanged chained tags must not refresh the playlist");
    seeks.clear();
}
}
int wmain(int argc, wchar_t** argv) {
    HMODULE dll = nullptr; HWND window = nullptr;
    try {
        Check(argc == 3, "usage: playback_notification_tests DLL DATA_DIR");
        RegistrySandbox sandbox;
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        const auto steady = directory / L"steady-cbr.opus";
        const auto first = directory / L"first.opus";
        const auto second = directory / L"second.opus";
        const auto chain = directory / L"changing.opus";
        MakeOpus(steady, 2, 96000, 8101, {}, 0, 96000);
        MakeOpus(first, 2, 48000, 8102, {}, 0, 96000);
        MakeOpus(second, 2, 48000, 8103, {"TITLE=Second track"}, 0, 192000);
        std::filesystem::copy_file(first, chain, std::filesystem::copy_options::overwrite_existing);
        AppendFile(chain, second);
        const auto unchanged = directory / L"unchanged.opus";
        MakeOpus(second, 2, 48000, 8104, {}, 0, 192000);
        std::filesystem::copy_file(first, unchanged, std::filesystem::copy_options::overwrite_existing);
        AppendFile(unchanged, second);
        const auto tags_only = directory / L"tags-only.opus";
        MakeOpus(second, 2, 48000, 8105, {"ALBUM=Second album"}, 0, 192000);
        std::filesystem::copy_file(first, tags_only, std::filesystem::copy_options::overwrite_existing);
        AppendFile(tags_only, second);

        WNDCLASSW cls{}; cls.lpfnWndProc = Host; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"OpusPlaybackNotificationTest";
        Check(RegisterClassW(&cls) != 0, "notification-test window class");
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        Check(window != nullptr, "notification-test host window");
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll != nullptr, "load notification-test DLL");
        const auto get = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        Check(get != nullptr, "notification-test input export");
        input = get(); input->hDllInstance = dll; input->hMainWindow = window;
        winamp::OutModule out{}; out.Open = Open; out.Close = Close; out.Write = Write; out.CanWrite = CanWrite;
        out.IsPlaying = Playing; out.Pause = Pause; out.Flush = Flush; out.GetOutputTime = Time; out.GetWrittenTime = Time;
        input->outMod = &out; input->SetInfo = SetInfo;
        for (const bool native : {false, true}) {
            RunSeeks(steady, native, 0);
            RunSeeks(unchanged, native, 0);
            RunSeeks(chain, native, 3);
            RunSeeks(tags_only, native, 3);
            Run(steady, native, true, false);
            Run(chain, native, true, true);
            Run(steady, native, false, false);
        }
        RunIdle(steady);
        input->Quit(); input = nullptr; FreeLibrary(dll); dll = nullptr;
        DestroyWindow(window); window = nullptr;
        std::cout << "Playback notifications: one initial format, bitrate-only changed values, no periodic title redraws, real chained title changes preserved.\n";
        return 0;
    } catch (const std::exception& e) {
        if (input) { input->Stop(); input = nullptr; }
        if (dll) FreeLibrary(dll);
        if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
