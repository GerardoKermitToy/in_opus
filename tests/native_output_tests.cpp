#include "fixtures.hpp"
#include "dialog_capture.hpp"
#include "registry_sandbox.hpp"
#include "opus_decoder.hpp"
#include "output_settings.hpp"
#include "resource.h"
#include "winamp_input.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

using namespace winamp_opus;
namespace {
// Independent expected Windows speaker order (not the production table).
constexpr int kExpectedOrder[8][8]{
    {0}, {0,1}, {0,2,1}, {0,1,2,3}, {0,2,1,3,4},
    {0,2,1,5,3,4}, {0,2,1,6,5,3,4}, {0,2,1,7,5,6,3,4}};
int Read(void* file, unsigned char* data, int bytes) {
    const auto n = std::fread(data, 1, bytes, static_cast<FILE*>(file));
    return n == 0 && std::ferror(static_cast<FILE*>(file)) ? -1 : static_cast<int>(n);
}
int Seek(void* file, opus_int64 offset, int origin) { return _fseeki64(static_cast<FILE*>(file), offset, origin); }
opus_int64 Tell(void* file) { return _ftelli64(static_cast<FILE*>(file)); }
int CloseFile(void* file) { return std::fclose(static_cast<FILE*>(file)); }

std::vector<short> Reference(const wchar_t* path, int channels, std::int64_t first = 0) {
    FILE* file = nullptr;
    Check(_wfopen_s(&file, path, L"rb") == 0, "open raw reference");
    std::unique_ptr<FILE, decltype(&std::fclose)> stream(file, &std::fclose);
    const OpusFileCallbacks callbacks{Read, Seek, Tell, CloseFile};
    int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> raw(
        op_open_callbacks(file, &callbacks, nullptr, 0, &error), &op_free);
    Check(raw != nullptr, "raw reference decoder");
    stream.release();
    if (first > 0) Check(op_pcm_seek(raw.get(), first) == 0, "reference seek");
    std::array<short, 576 * 8> block{};
    std::vector<short> samples;
    for (;;) {
        // Match the public decoder's 576-frame block boundary, independently
        // decode in Opus order, then explicitly select Windows speaker slots.
        int frames = 0;
        while (frames < 576) {
            const int count = op_read(raw.get(), block.data() + frames * channels,
                (576 - frames) * channels, nullptr);
            Check(count >= 0, "raw decode");
            if (!count) break;
            frames += count;
        }
        if (!frames) break;
        for (int i = 0; i < frames; ++i)
            for (int ch = 0; ch < channels; ++ch)
                samples.push_back(block[i * channels + kExpectedOrder[channels - 1][ch]]);
    }
    return samples;
}

void Compare(const std::vector<short>& actual, const std::vector<short>& expected) {
    Check(actual.size() == expected.size(), "native PCM sample count");
    int difference = 0;
    for (std::size_t i = 0; i < actual.size(); ++i)
        difference = std::max(difference, std::abs(static_cast<int>(actual[i]) - expected[i]));
    Check(difference < 32, "native PCM channel order/content differs from raw reference");
}
void ValidateDecoder(const wchar_t* path) {
    Decoder decoder;
    Check(decoder.Open(path), "open native production decoder");
    const int channels = decoder.Info().source_channels;
    Check(decoder.SetOutputMode(OutputMode::SourceChannels), "select source-channel decoder");
    Check(decoder.OutputChannels() == channels, "decoder output channels");
    std::array<short, 576 * 8 + 2> guarded{};
    auto decode = [&] {
        std::vector<short> samples;
        for (;;) {
            guarded[0] = 12345;
            guarded[576 * channels + 1] = -12345;
            const int n = decoder.ReadFrames(guarded.data() + 1, 576);
            Check(n >= 0, "native production decode");
            Check(guarded[0] == 12345 && guarded[576 * channels + 1] == -12345, "native buffer overrun");
            if (!n) break;
            samples.insert(samples.end(), guarded.begin() + 1, guarded.begin() + 1 + n * channels);
        }
        return samples;
    };
    Compare(decode(), Reference(path, channels));
    const auto middle = decoder.Info().total_frames / 2;
    Check(decoder.SeekFrame(middle), "native sample-exact seek");
    Compare(decode(), Reference(path, channels, middle));
    Check(decoder.SeekFrame(decoder.Info().total_frames), "native seek EOF");
    Check(decoder.ReadFrames(guarded.data(), 576) == 0, "native forced EOF");
    Check(decoder.SetOutputMode(OutputMode::Stereo) && decoder.OutputChannels() == 2, "decoder stereo reset");
    std::cout << "Native decoder: " << channels << " channels, correct Windows order, seek, tail, guards.\n";
}

winamp::InModule* input = nullptr;
std::atomic<int> errors{0}, eof{0}, flushes{0}, vis_calls{0}, dsp_calls{0};
std::atomic<std::int64_t> accepted_frames{0};
std::atomic<bool> writable{true}, reject_once{false}, hold_drain{false}, dsp_active{false}, dsp_double{false};
std::vector<short> captured;
std::vector<int> open_requests;
int channels = 2, expected_channels = 2, capacity = 8192, successful_opens = 0, closes = 0;
bool reject_native = false, reject_all = false;

void Verify(bool condition) { if (!condition) ++errors; }
LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == winamp::kEndOfFile) { ++eof; return 0; }
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Wait(const std::function<bool()>& condition, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    while (!condition()) { Pump(); Check(GetTickCount64() < deadline, message); Sleep(1); }
    Pump();
}
int __cdecl Open(int rate, int count, int bits, int, int) {
    Verify(rate == 48000 && bits == 16 && count >= 1 && count <= 8);
    open_requests.push_back(count);
    if (reject_all || (reject_native && count != 2)) return -1;
    channels = count; ++successful_opens;
    return 20;
}
void __cdecl Close() { ++closes; }
int __cdecl Write(char* data, int bytes) {
    Verify(data && bytes > 0 && bytes <= 8192 && bytes <= capacity && bytes % (channels * 2) == 0);
    if (reject_once.exchange(false)) return 1;
    const auto* samples = reinterpret_cast<const short*>(data);
    captured.insert(captured.end(), samples, samples + bytes / 2);
    accepted_frames += bytes / (channels * 2);
    return 0;
}
int __cdecl CanWrite() { return writable ? capacity : 0; }
int __cdecl IsPlaying() { return hold_drain ? 1 : 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl Flush(int) { captured.clear(); accepted_frames = 0; ++flushes; }
int __cdecl Time() { return 0; }
void __cdecl VisInit(int latency, int rate) { Verify(latency == 20 && rate == 48000); }
void __cdecl VisDeinit() { }
void __cdecl VisInfo(int rate, int count) { Verify(rate == 48000 && count == expected_channels); }
void __cdecl Vis(void* pcm, int count, int bits, int) {
    Verify(count == expected_channels && bits == 16);
    // Match stock in_wave/in_vorbis: a full block in the actual channel format.
    const volatile short tail = static_cast<short*>(pcm)[576 * count - 1];
    (void)tail;
    ++vis_calls;
}
void __cdecl SetInfo(int bitrate, int rate, int count, int sync) {
    Verify(bitrate > 0 && rate == 48 && count == expected_channels && sync == 1);
}
int __cdecl DspActive() { return dsp_active ? 1 : 0; }
int __cdecl Dsp(short* pcm, int frames, int bits, int count, int rate) {
    Verify(count == expected_channels && bits == 16 && rate == 48000);
    ++dsp_calls;
    if (!dsp_double) return frames;
    for (int i = frames - 1; i >= 0; --i)
        for (int ch = 0; ch < count; ++ch) {
            const short value = pcm[i * count + ch];
            pcm[(i * 2) * count + ch] = pcm[(i * 2 + 1) * count + ch] = value;
        }
    return frames * 2;
}
void Reset(int count) {
    Pump(); captured.clear(); open_requests.clear();
    expected_channels = count; errors = 0; eof = 0; flushes = 0; vis_calls = 0; dsp_calls = 0;
    writable = true; reject_once = false; hold_drain = false; dsp_active = false; dsp_double = false;
    accepted_frames = 0;
    reject_native = reject_all = false; capacity = 8192;
}
void StopAndVerify() {
    input->Stop();
    Check(errors == 0, "multichannel host callback violation");
    Check(successful_opens == closes, "output resource imbalance");
}

HHOOK hook = nullptr;
int config_initial = IDC_OUTPUT_STEREO, config_select = IDC_OUTPUT_SOURCE, config_close = IDOK;
bool dialog_seen = false;
bool inspect_info = false;
std::wstring expected_output, expected_note;
std::string dialog_error;
std::filesystem::path screenshot_directory;
bool config_captured = false, info_captured = false;
std::wstring Text(HWND dialog, int id) {
    wchar_t text[1024]{};
    GetDlgItemTextW(dialog, id, text, 1024);
    return text;
}
void CALLBACK InspectDialog(HWND dialog, UINT, UINT_PTR timer, DWORD) noexcept {
    KillTimer(dialog, timer);
    try {
        wchar_t caption[128]{};
        GetWindowTextW(dialog, caption, 128);
        if (inspect_info) {
            Check(std::wstring(caption) == L"Opus File Information", "English properties caption");
            Check(Text(dialog, IDOK) == L"&Close", "English Close button");
            Check(Text(dialog, IDC_INFO_OUTPUT) == expected_output, "properties output format");
            Check(Text(dialog, IDC_INFO_STATUS).find(expected_note) != std::wstring::npos, "properties fallback reason");
        } else {
            Check(std::wstring(caption) == L"Winamp Opus Settings", "English settings caption");
            Check(Text(dialog, IDC_OUTPUT_STEREO) == L"&Stereo", "English stereo option");
            Check(Text(dialog, IDC_OUTPUT_SOURCE) == L"S&ource channels (default)", "English source-channel option");
            Check(Text(dialog, IDOK) == L"&OK" && Text(dialog, IDCANCEL) == L"&Cancel", "English settings buttons");
            Check(IsDlgButtonChecked(dialog, config_initial) == BST_CHECKED, "configuration initial selection");
            CheckRadioButton(dialog, IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE, config_select);
        }
        RECT client{}; GetClientRect(dialog, &client);
        for (int id : {IDOK, inspect_info ? IDC_INFO_STATUS : IDC_OUTPUT_STATUS}) {
            RECT rect{}; GetWindowRect(GetDlgItem(dialog, id), &rect);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
            Check(rect.right <= client.right && rect.bottom <= client.bottom, "dialog control clipped");
        }
        if (!screenshot_directory.empty() && !inspect_info && !config_captured) {
            CaptureDialog(dialog, (screenshot_directory / L"config.bmp").c_str());
            config_captured = true;
        } else if (!screenshot_directory.empty() && inspect_info && !info_captured && expected_note == L"Source channels") {
            CaptureDialog(dialog, (screenshot_directory / L"native-info.bmp").c_str());
            info_captured = true;
        }
        dialog_seen = true;
    } catch (const std::exception& error) { dialog_error = error.what(); }
    PostMessageW(dialog, WM_COMMAND, inspect_info ? IDOK : config_close, 0);
}
LRESULT CALLBACK DialogHook(int code, WPARAM w, LPARAM l) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(l);
        if (message.message == WM_INITDIALOG &&
            GetDlgItem(message.hwnd, inspect_info ? IDC_INFO_OUTPUT : IDC_OUTPUT_SOURCE))
            SetTimer(message.hwnd, 74, 20, InspectDialog);
    }
    return CallNextHookEx(hook, code, w, l);
}
void WithDialog(const std::function<void()>& show) {
    dialog_seen = false; dialog_error.clear();
    hook = SetWindowsHookExW(WH_CALLWNDPROCRET, DialogHook, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "test dialog hook");
    show();
    UnhookWindowsHookEx(hook); hook = nullptr;
    Check(dialog_error.empty(), dialog_error.c_str());
    Check(dialog_seen, "configuration/properties dialog not inspected");
}
void Config(int initial, int selected, int close = IDOK) {
    inspect_info = false;
    config_initial = initial; config_select = selected; config_close = close;
    WithDialog([] { input->Config(nullptr); });
}
void Properties(int count, const wchar_t* reason) {
    inspect_info = true; expected_output = OutputFormatText(count); expected_note = reason;
    WithDialog([] { input->InfoBox(nullptr, nullptr); });
    inspect_info = false;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    HMODULE library = nullptr;
    HWND window = nullptr;
    try {
        Check(argc == 3 || argc == 4, "usage: native_output_tests DLL directory [surround.opus]");
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        if (GetEnvironmentVariableW(L"WINAMP_OPUS_CAPTURE_DIALOGS", nullptr, 0) > 0)
            screenshot_directory = directory;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        std::array<std::filesystem::path, 8> files;
        for (int count = 1; count <= 8; ++count) {
            files[count - 1] = directory / (std::to_wstring(count) + L"-каналов.opus");
            MakeOpus(files[count - 1], count, 12017, 600 + count);
            ValidateDecoder(files[count - 1].c_str());
        }
        if (argc == 4) ValidateDecoder(argv[3]);
        for (int count = 1; count <= 8; ++count) {
            std::array<short, 8> ramp{};
            for (int ch = 0; ch < count; ++ch) ramp[ch] = static_cast<short>(1000 + ch * 100);
            ReorderToWindows(ramp.data(), 1, count);
            for (int ch = 0; ch < count; ++ch)
                Check(ramp[ch] == 1000 + kExpectedOrder[count - 1][ch] * 100, "explicit speaker-slot permutation");
        }

        WNDCLASSW cls{}; cls.lpfnWndProc = HostProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"WinampOpusNativeTestHost";
        Check(RegisterClassW(&cls) != 0, "register native host");
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        Check(window != nullptr, "native host window");
        library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(library != nullptr, "load native plugin DLL");
        const auto get_module = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(library, "winampGetInModule2"));
        Check(get_module != nullptr, "native input export");
        input = get_module(); input->hDllInstance = library; input->hMainWindow = window;
        winamp::OutModule output{};
        output.Open = Open; output.Close = Close; output.Write = Write; output.CanWrite = CanWrite;
        output.IsPlaying = IsPlaying; output.Pause = Pause; output.Flush = Flush;
        output.GetOutputTime = Time; output.GetWrittenTime = Time;
        input->outMod = &output;
        input->SAVSAInit = VisInit; input->SAVSADeInit = VisDeinit;
        input->SAAddPCMData = Vis; input->VSAAddPCMData = Vis; input->VSASetInfo = VisInfo;
        input->SetInfo = SetInfo; input->dsp_isactive = DspActive; input->dsp_dosamples = Dsp;
        Check(OutputFormatText(6) == L"48,000 Hz, 16-bit, 5.1 (6 channels)", "English 5.1 format description");
        Check(OutputFormatText(8) == L"48,000 Hz, 16-bit, 7.1 (8 channels)", "English 7.1 format description");
        Check(LoadOutputMode() == OutputMode::SourceChannels, "missing setting must default to source channels");
        registry.StoreOutputMode(99);
        Check(LoadOutputMode() == OutputMode::SourceChannels, "invalid setting must default to source channels");
        registry.StoreOutputMode(static_cast<DWORD>(OutputMode::Stereo));
        Check(LoadOutputMode() == OutputMode::Stereo, "explicit stereo preference must be respected");
        Check(RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kOutputModeValue) == ERROR_SUCCESS,
            "clear sandbox setting for fresh-start test");
        input->Init();
        // A fresh start must actually send six channels, without saving any setting.
        Reset(6);
        Check(input->Play(files[5].c_str()) == 0, "default source-channel playback");
        Wait([] { return eof == 1; }, "default source-channel EOF");
        StopAndVerify();
        Check(open_requests == std::vector<int>{6}, "default playback must open source channels");
        Compare(captured, Reference(files[5].c_str(), 6));
        Config(IDC_OUTPUT_SOURCE, IDC_OUTPUT_STEREO, IDCANCEL);
        Check(LoadOutputMode() == OutputMode::SourceChannels, "cancel changed default setting");
        Config(IDC_OUTPUT_SOURCE, IDC_OUTPUT_STEREO);
        Check(LoadOutputMode() == OutputMode::Stereo, "explicit stereo setting persisted");
        input->Quit(); input->Init();
        Config(IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE);
        Check(LoadOutputMode() == OutputMode::SourceChannels, "source setting persisted");
        input->Quit(); input->Init();
        Config(IDC_OUTPUT_SOURCE, IDC_OUTPUT_STEREO, IDCANCEL);

        for (int count = 1; count <= 8; ++count) {
            Reset(count); reject_once = true; dsp_active = true;
            // Small, deliberately non-frame-aligned capacity exercises chunks
            // and would deadlock an implementation waiting for a full block.
            capacity = count == 8 ? 1001 : 8192;
            Check(input->Play(files[count - 1].c_str()) == 0, "play native channel layout");
            Wait([] { return eof == 1; }, "native EOF");
            StopAndVerify();
            Check(open_requests == std::vector<int>{count}, "unexpected native Open format");
            Compare(captured, Reference(files[count - 1].c_str(), count));
            Check(vis_calls > 0 && dsp_calls > 0, "real-channel DSP or visualization callbacks");
        }
        for (const int count : {6, 8}) {
            Reset(count); dsp_active = true; dsp_double = true; capacity = 1001;
            Check(input->Play(files[count - 1].c_str()) == 0, "multichannel 2x DSP playback");
            Wait([] { return eof == 1; }, "multichannel DSP EOF"); StopAndVerify();
            const auto raw = Reference(files[count - 1].c_str(), count);
            std::vector<short> doubled;
            for (std::size_t i = 0; i < raw.size(); i += count)
                for (int repeat = 0; repeat < 2; ++repeat)
                    doubled.insert(doubled.end(), raw.begin() + i, raw.begin() + i + count);
            Compare(captured, doubled);
        }
        if (argc == 4) {
            Decoder info; Check(info.Open(argv[3]), "external native info");
            const int count = info.Info().source_channels;
            Reset(count); capacity = 1001;
            Check(input->Play(argv[3]) == 0, "external native playback");
            Wait([] { return eof == 1; }, "external native EOF");
            Properties(count, L"Source channels");
            StopAndVerify(); Compare(captured, Reference(argv[3], count));
            std::cout << "External native output: " << count << " channels, " << captured.size() / count << " frames.\n";
        }

        Reset(6); writable = false;
        Check(input->Play(files[5].c_str()) == 0, "native paused playback");
        input->Pause(); Check(input->IsPaused() == 1, "native pause");
        input->SetOutputTime(73);
        Wait([] { return flushes == 1; }, "native seek paused");
        Properties(6, L"Source channels");
        // Saving a new mode while paused must not reinterpret the active PCM.
        Config(IDC_OUTPUT_SOURCE, IDC_OUTPUT_STEREO);
        writable = true; input->UnPause();
        Wait([] { return eof == 1; }, "native seek EOF");
        StopAndVerify(); Compare(captured, Reference(files[5].c_str(), 6, 73 * 48));
        Reset(2);
        Check(input->Play(files[5].c_str()) == 0, "stereo takes effect on next Play");
        Wait([] { return eof == 1; }, "next-play stereo EOF"); StopAndVerify();
        Check(open_requests == std::vector<int>{2}, "new stereo setting not applied");
        Config(IDC_OUTPUT_STEREO, IDC_OUTPUT_SOURCE);

        Reset(8); hold_drain = true;
        Check(input->Play(files[7].c_str()) == 0, "native EOF seek start");
        Wait([] { return accepted_frames == 12017; }, "native all frames written");
        Properties(8, L"Source channels");
        Check(eof == 0, "native EOF before output drain");
        hold_drain = false; Wait([] { return eof == 1; }, "native drained EOF");
        input->Pause(); input->SetOutputTime(50);
        Wait([] { return flushes == 1; }, "native seek after EOF");
        input->UnPause(); Wait([] { return eof == 2; }, "native second EOF");
        StopAndVerify(); Compare(captured, Reference(files[7].c_str(), 8, 50 * 48));

        Reset(2); reject_native = true; hold_drain = true;
        Check(input->Play(files[5].c_str()) == 0, "fallback Open");
        Check(open_requests == std::vector<int>({6, 2}), "native reject must retry stereo");
        Properties(2, L"rejected");
        hold_drain = false; Wait([] { return eof == 1; }, "fallback EOF"); StopAndVerify();
        Decoder stereo; Check(stereo.Open(files[5].c_str()), "stereo fallback reference");
        std::array<short, 1152> block{}; std::vector<short> expected;
        for (int n; (n = stereo.ReadFrames(block.data(), 576)) > 0;)
            expected.insert(expected.end(), block.begin(), block.begin() + n * 2);
        Compare(captured, expected);

        Reset(2); reject_all = true;
        Check(input->Play(files[5].c_str()) != 0, "both Open failures must fail Play");
        StopAndVerify(); Check(open_requests == std::vector<int>({6,2}), "both failures bounded retry");
        const auto chained = directory / L"changing-layout.opus";
        std::filesystem::copy_file(files[5], chained, std::filesystem::copy_options::overwrite_existing);
        AppendFile(chained, files[7]);
        Decoder chain; Check(chain.Open(chained.c_str()), "mixed chain open");
        Check(chain.Info().native_channels == 0 && !chain.SetOutputMode(OutputMode::SourceChannels), "mixed chain must reject native");
        Reset(2); hold_drain = true;
        Check(input->Play(chained.c_str()) == 0, "mixed chain stereo fallback");
        Properties(2, L"layout");
        hold_drain = false; Wait([] { return eof == 1; }, "mixed chain EOF"); StopAndVerify();
        Check(captured.size() == 12017 * 2 * 2, "mixed chain frame count");
        const auto same = directory / L"same-layout.opus";
        std::filesystem::copy_file(files[5], same, std::filesystem::copy_options::overwrite_existing);
        const auto six_again = directory / L"six-again.opus";
        MakeOpus(six_again, 6, 2417, 699); AppendFile(same, six_again);
        ValidateDecoder(same.c_str());
        Reset(6); Check(input->Play(same.c_str()) == 0, "same-layout native chain");
        Wait([] { return eof == 1; }, "same-layout chain EOF"); StopAndVerify();
        Compare(captured, Reference(same.c_str(), 6));

        Config(IDC_OUTPUT_SOURCE, IDC_OUTPUT_STEREO);
        input->Quit(); FreeLibrary(library); library = nullptr; input = nullptr;
        DestroyWindow(window); window = nullptr;
        std::cout << "Native output passed: 1..8 channels, speaker order, chunk limits, real-channel DSP/visualization, 2x DSP,\n"
                     "pause/seek/EOF/drain, default source channels, stereo fallback, chains, settings persistence/Cancel/next-Play.\n";
        return 0;
    } catch (const std::exception& error) {
        if (hook) UnhookWindowsHookEx(hook);
        if (input) input->Stop();
        if (library) FreeLibrary(library);
        if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
