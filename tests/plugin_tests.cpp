#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "opus_decoder.hpp"
#include "winamp_input.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::atomic<std::int64_t> written_frames{0};
std::atomic<std::int64_t> frames_since_flush{0};
std::atomic<int> eof_messages{0}, flushes{0}, last_flush{-1}, writes{0};
std::atomic<int> last_volume{0}, last_pan{0}, errors{0}, visualizations{0};
std::atomic<bool> writable{true}, hold_end{false}, reject_write{false}, double_dsp{false};
bool fail_open = false;
int opens = 0, closes = 0, vis_inits = 0, vis_deinits = 0;
winamp::InModule* input = nullptr;

void Verify(bool condition) { if (!condition) ++errors; }
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == winamp::kEndOfFile) { ++eof_messages; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
void Wait(const std::function<bool()>& condition, const char* failure, DWORD timeout = 10000) {
    const auto deadline = GetTickCount64() + timeout;
    while (!condition()) {
        Pump();
        Check(GetTickCount64() < deadline, failure);
        Sleep(1);
    }
    Pump();
}
void For(DWORD milliseconds) {
    const auto deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline) { Pump(); Sleep(1); }
}
int __cdecl Open(int rate, int channels, int bits, int, int) {
    Verify(rate == 48000 && channels == 2 && bits == 16);
    if (fail_open) return -1;
    ++opens;
    return 20;
}
void __cdecl Close() { ++closes; }
int __cdecl Write(char* data, int bytes) {
    Verify(data && bytes > 0 && bytes <= 8192 && bytes % 4 == 0);
    if (reject_write.exchange(false)) return 1;
    written_frames += bytes / 4;
    frames_since_flush += bytes / 4;
    ++writes;
    return 0;
}
int __cdecl CanWrite() { return writable ? 8192 : 0; }
int __cdecl IsPlaying() { return hold_end ? 1 : 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl Volume(int value) { last_volume = value; }
void __cdecl Pan(int value) { last_pan = value; }
void __cdecl Flush(int value) { frames_since_flush = 0; last_flush = value; ++flushes; }
int __cdecl Time() { return static_cast<int>(frames_since_flush.load() / 48); }
void __cdecl VisInit(int latency, int rate) { Verify(latency == 20 && rate == 48000); ++vis_inits; }
void __cdecl VisDeinit() { ++vis_deinits; }
void __cdecl Vis(void* data, int channels, int bits, int timestamp) {
    Verify(data && channels == 2 && bits == 16 && timestamp >= 0);
    // Touch the last sample: the ABI promises a buffer of at least 576 frames.
    const volatile short tail = static_cast<short*>(data)[575 * 2 + 1];
    (void)tail;
    ++visualizations;
}
void __cdecl VisInfo(int rate, int channels) { Verify(rate == 48000 && channels == 2); }
void __cdecl SetInfo(int bitrate, int rate, int channels, int sync) {
    Verify(bitrate > 0 && rate == 48 && channels == 2 && sync == 1);
}
int __cdecl DspActive() { return double_dsp ? 1 : 0; }
int __cdecl Dsp(short* pcm, int frames, int bits, int channels, int rate) {
    Verify(frames > 0 && frames <= 576 && bits == 16 && channels == 2 && rate == 48000);
    for (int i = frames - 1; i >= 0; --i) {
        const short left = pcm[i * 2], right = pcm[i * 2 + 1];
        pcm[i * 4] = pcm[i * 4 + 2] = left;
        pcm[i * 4 + 1] = pcm[i * 4 + 3] = right;
    }
    return frames * 2;
}
void Reset() {
    Pump();
    written_frames = 0; frames_since_flush = 0; eof_messages = 0;
    flushes = 0; last_flush = -1; writes = 0; visualizations = 0;
    writable = true; hold_end = false; reject_write = false; double_dsp = false;
}
void Finished() {
    input->Stop();
    Check(opens == closes && vis_inits == vis_deinits, "resource lifetime imbalance");
    Check(errors == 0, "callback contract violation");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    registry.StoreOutputMode(static_cast<DWORD>(winamp_opus::OutputMode::Stereo));
    HMODULE library = nullptr;
    HWND window = nullptr;
    try {
        Check(argc == 3 || argc == 4, "usage: plugin_tests DLL directory [sample.opus]");
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        auto path = directory / L"Тест плагина.opus";
        if (argc == 4) path = argv[3];
        else MakeOpus(path, 2, 144117, 201);
        winamp_opus::Decoder reference;
        Check(reference.Open(path.c_str()), "open reference file");
        const auto info = reference.Info();
        const int seek_target = std::min(1000, info.length_ms / 2);
        reference.Close();

        WNDCLASSW cls{};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"WinampOpusTestHost";
        Check(RegisterClassW(&cls) != 0, "register window class");
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        Check(window != nullptr, "create message-only window");
        library = LoadLibraryExW(argv[1], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(library != nullptr, "load plugin DLL (and static dependencies)");
        const auto get_module = reinterpret_cast<winamp::InModule* (__cdecl*)()>(
            GetProcAddress(library, "winampGetInModule2"));
        using Metadata = int (__cdecl*)(const wchar_t*, const char*, wchar_t*, std::size_t);
        const auto metadata = reinterpret_cast<Metadata>(GetProcAddress(library, "winampGetExtendedFileInfoW"));
        Check(get_module && metadata, "undecorated exports");
        input = get_module();
        Check(input && input->version == winamp::kInputVersion, "Unicode input ABI");
        Check(std::string(input->FileExtensions) == "opus", "extension registration");
        winamp::OutModule output{};
        output.Open = Open; output.Close = Close; output.Write = Write; output.CanWrite = CanWrite;
        output.IsPlaying = IsPlaying; output.Pause = Pause; output.SetVolume = Volume;
        output.SetPan = Pan; output.Flush = Flush; output.GetOutputTime = Time; output.GetWrittenTime = Time;
        input->outMod = &output; input->hMainWindow = window; input->hDllInstance = library;
        input->SAVSAInit = VisInit; input->SAVSADeInit = VisDeinit;
        input->SAAddPCMData = Vis; input->VSAAddPCMData = Vis; input->VSASetInfo = VisInfo;
        input->SetInfo = SetInfo; input->dsp_isactive = DspActive; input->dsp_dosamples = Dsp;
        input->Init();
        wchar_t title[2048]{};
        int duration = 0;
        input->GetFileInfo(path.c_str(), title, &duration);
        Check(duration == info.length_ms && title == info.display_title, "GetFileInfo title/duration");
        Check(metadata(path.c_str(), "length", title, 2048) && std::stoi(title) == info.length_ms, "extended length");
        Check(!metadata(path.c_str(), "unknown-field", title, 2048), "unknown metadata key");
        wchar_t tiny[1]{L'x'};
        Check(metadata(path.c_str(), "family", tiny, 1) && tiny[0] == 0, "bounded metadata copy");
        Check(input->IsOurFile(path.c_str()) && input->IsOurFile(L"song.OPUS"), "case insensitive extension");
        Check(!input->IsOurFile(L"http://example.invalid/song.opus"), "do not claim URLs");
        Check(!input->IsOurFile(L"song.ogg") && !input->IsOurFile(L"song.oga"), "do not claim missing Ogg files");
        Check(input->Play((directory / L"missing.opus").c_str()) != 0, "missing file failure");
        fail_open = true;
        Check(input->Play(path.c_str()) != 0, "output open failure");
        fail_open = false;

        const auto probe_source = directory / L"probe.opus";
        MakeOpus(probe_source, 2, 2417, 202);
        for (const auto* extension : {L".ogg", L".OGG", L".oga", L".OGA"}) {
            const auto opus_container = directory / (std::wstring(L"Контейнер Opus") + extension);
            std::filesystem::copy_file(probe_source, opus_container, std::filesystem::copy_options::overwrite_existing);
            Check(input->IsOurFile(opus_container.c_str()) == 1, "recognize actual Opus in .ogg/.oga");
            Reset();
            Check(input->Play(opus_container.c_str()) == 0, "play Opus in .ogg/.oga");
            Wait([] { return eof_messages == 1; }, "Ogg Opus container EOF");
            Check(written_frames == 2417, "Ogg Opus container sample count");
            Finished();
            const auto vorbis = directory / (std::wstring(L"vorbis") + extension);
            MakeVorbisHeaders(vorbis, 203);
            Check(input->IsOurFile(vorbis.c_str()) == 0, "do not claim Vorbis headers");
        }
        const auto fake = directory / L"fake.oga";
        { std::ofstream file(fake, std::ios::binary); file << "OggS not a real Ogg page; OpusHead"; }
        Check(!input->IsOurFile(fake.c_str()), "do not recognize signature text without valid Ogg framing");
        Check(!input->IsOurFile(nullptr), "null filename probe");

        Reset(); writable = false;
        Check(input->Play(path.c_str()) == 0, "start playback");
        input->Pause();
        Check(input->IsPaused() == 1 && input->GetLength() == info.length_ms, "pause/length");
        For(30);
        Check(writes == 0, "paused or blocked playback writes audio");
        input->SetVolume(123); input->SetPan(-40);
        Check(last_volume == 123 && last_pan == -40, "volume and pan forwarding");
        input->SetOutputTime(seek_target);
        Wait([] { return flushes > 0; }, "seek while paused");
        Check(last_flush == seek_target && input->GetOutputTime() == seek_target, "seek timestamp");
        input->GetFileInfo(nullptr, title, &duration);
        Check(duration == info.length_ms && title == info.display_title, "current track metadata");
        input->Stop(); // Must also work while paused/backpressured.
        Finished();

        Reset(); reject_write = true; hold_end = true;
        Check(input->Play(path.c_str()) == 0, "restart playback");
        Wait([&] { return written_frames == info.total_frames; }, "decode to output");
        For(25);
        Check(eof_messages == 0, "EOF before output drain");
        hold_end = false;
        Wait([] { return eof_messages == 1; }, "EOF notification");
        Check(visualizations > 0, "visualization callbacks");
        Check(input->GetOutputTime() <= info.length_ms, "end timestamp bound");
        input->SetOutputTime(seek_target);
        Wait([] { return flushes > 0; }, "seek after EOF");
        Wait([] { return eof_messages == 2; }, "EOF after second seek");
        Check(written_frames == info.total_frames * 2 - static_cast<std::int64_t>(seek_target) * 48,
              "seek after EOF sample count");
        Finished();

        Reset(); double_dsp = true;
        Check(input->Play(path.c_str()) == 0, "DSP playback");
        Wait([] { return eof_messages == 1; }, "DSP EOF");
        Check(written_frames == info.total_frames * 2, "2x DSP buffer/sample count");
        Finished();
        input->Quit();
        FreeLibrary(library); library = nullptr; input = nullptr;
        DestroyWindow(window); window = nullptr;
        std::cout << "Plugin integration passed: frames=" << info.total_frames
                  << "; exports, metadata, pause, seek, EOF, backpressure, DSP, visualization, cleanup.\n";
        return 0;
    } catch (const std::exception& error) {
        if (input) input->Stop();
        if (library) FreeLibrary(library);
        if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
