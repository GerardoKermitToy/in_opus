#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "output_settings.hpp"
#include "opus_decoder.hpp"
#include "winamp_input.hpp"
#include <atomic>
#include <iostream>

using namespace winamp_opus;
namespace {
std::atomic<int> errors{0}, eof{0}, vis{0}, dsp{0};
std::atomic<std::int64_t> written{0};
int expected_rate = 48000, expected_channels = 2, current_channels = 2;
bool reject_surround = false;
void Verify(bool value) { if (!value) ++errors; }
int __cdecl Open(int rate, int channels, int bits, int, int) {
    Verify(rate == expected_rate && bits == 24);
    if (reject_surround && channels != 2) return -1;
    Verify(channels == expected_channels); current_channels = channels; return 10;
}
void __cdecl Close() {}
int __cdecl Write(char* data, int bytes) {
    Verify(data && bytes > 0 && bytes <= 8192 && bytes % (current_channels * 3) == 0);
    written += bytes / (current_channels * 3); return 0;
}
int __cdecl CanWrite() { return 8192; }
int __cdecl Playing() { return 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl Flush(int) { written = 0; }
int __cdecl Time() { return 0; }
void __cdecl VisInit(int latency, int rate) { Verify(latency == 10 && rate == expected_rate); }
void __cdecl VisClose() {}
void __cdecl VisInfo(int rate, int channels) { Verify(rate == expected_rate && channels == expected_channels); }
void __cdecl Visual(void* pcm, int channels, int bits, int) { Verify(pcm && channels == expected_channels && bits == 16); ++vis; }
void __cdecl SetInfo(int bitrate, int rate, int channels, int sync) {
    Verify(bitrate >= 0 && sync == 1 && ((rate == expected_rate / 1000 && channels == expected_channels) ||
        (rate == -1 && channels == -1)));
}
int __cdecl DspActive() { return 1; }
int __cdecl Dsp(short* pcm, int frames, int bits, int channels, int rate) {
    Verify(pcm && frames > 0 && frames <= 576 && bits == 24 && channels == expected_channels && rate == expected_rate); ++dsp;
    return frames;
}
LRESULT CALLBACK Host(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == winamp::kEndOfFile) { ++eof; return 0; }
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
}
int wmain(int argc, wchar_t** argv) {
    HMODULE dll = nullptr; HWND window = nullptr; winamp::InModule* input = nullptr;
    try {
        Check(argc == 3, "usage: advanced_plugin_tests DLL NATIVE_FIXTURES_DIR");
        RegistrySandbox sandbox;
        WNDCLASSW cls{}; cls.lpfnWndProc = Host; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"AdvancedOpusPluginTest";
        Check(RegisterClassW(&cls) != 0, "advanced host class");
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        Check(window != nullptr, "advanced host window");
        dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(dll != nullptr, "load advanced DLL");
        const auto get = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(dll, "winampGetInModule2"));
        const auto metadata = reinterpret_cast<int (__cdecl*)(const wchar_t*, const char*, wchar_t*, int)>(GetProcAddress(dll, "winampGetExtendedFileInfoW"));
        Check(get && metadata, "advanced DLL exports");
        input = get(); input->hDllInstance = dll; input->hMainWindow = window;
        winamp::OutModule out{}; out.Open = Open; out.Close = Close; out.Write = Write; out.CanWrite = CanWrite;
        out.IsPlaying = Playing; out.Pause = Pause; out.Flush = Flush; out.GetOutputTime = Time; out.GetWrittenTime = Time;
        input->outMod = &out; input->SAVSAInit = VisInit; input->SAVSADeInit = VisClose; input->VSASetInfo = VisInfo;
        input->SAAddPCMData = Visual; input->VSAAddPCMData = Visual; input->SetInfo = SetInfo;
        input->dsp_isactive = DspActive; input->dsp_dosamples = Dsp;
        const std::filesystem::path directory(argv[2]);
        for (int scenario = 0; scenario < 4; ++scenario) {
            PlaybackSettings s; s.sample_rate = scenario == 1 ? 48000 : 0; s.bits = 24; s.dither = false;
            s.ambisonic = AmbisonicMode::Surround51;
            const auto path = directory / (scenario < 2 ? L"hd.opus" : L"ambisonic.opus");
            Check(std::filesystem::is_regular_file(path), "native fixture must exist");
            Decoder reference; Check(reference.Open(path.c_str(), nullptr, s), "reference advanced decoder");
            expected_rate = scenario == 1 ? 48000 : reference.SampleRate();
            expected_channels = scenario == 2 ? 6 : 2; reject_surround = scenario == 3;
            const auto expected_frames = reference.Info().total_frames * expected_rate / 48000;
            Check(SavePlaybackSettings(s), "save isolated DLL decoder settings"); input->Init();
            errors = eof = vis = dsp = 0; written = 0;
            Check(input->Play(path.c_str()) == 0, "advanced DLL Play");
            const auto deadline = GetTickCount64() + 10000;
            while (!eof && GetTickCount64() < deadline) { Pump(); Sleep(1); }
            Pump(); Check(eof == 1, "advanced DLL EOF");
            wchar_t info[8192]{};
            Check(metadata(nullptr, "formatinformation", info, 8192) == 1, "active advanced diagnostics export");
            Check(std::wstring(info).find(L"Last packet duration:") != std::wstring::npos, "packet diagnostics available after playback");
            input->Stop();
            Check(errors == 0 && vis > 0 && dsp > 0, "native rate/layout DSP/visualization negotiation");
            Check(written == expected_frames, "advanced DLL exact output length");
        }
        input->Quit(); input = nullptr; FreeLibrary(dll); dll = nullptr; DestroyWindow(window); window = nullptr;
        std::cout << "Advanced DLL: native HD96k, explicit 48k resampling, Ambisonics 5.1, stereo fallback, metadata, DSP/visualization and exact lengths passed.\n";
        return 0;
    } catch (const std::exception& e) {
        if (input) input->Stop(); if (dll) FreeLibrary(dll); if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
