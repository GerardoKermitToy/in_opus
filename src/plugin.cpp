#include "opus_decoder.hpp"
#include "file_info_dialog.hpp"
#include "output_settings.hpp"
#include "winamp_input.hpp"

#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace winamp_opus;

namespace {
winamp::InModule module{};
std::unique_ptr<Decoder> decoder;
HANDLE stop_event = nullptr;
HANDLE decode_thread = nullptr;
bool output_open = false;
bool visualization_open = false;
std::atomic<bool> paused{false};
std::atomic<int> seek_ms{-1};
std::atomic<int> decoded_ms{0};
std::atomic<int> length_ms{0};
int volume = -666; // Winamp output-plugin convention: retain the host's volume.
int pan = 0;
std::mutex metadata_mutex;
TrackInfo current_info;
std::wstring current_path;
std::wstring current_output_note;
int active_channels = 0; // Protected by metadata_mutex, as are the strings above.
std::atomic<OutputMode> output_mode{kDefaultOutputMode};
constexpr int kBlockFrames = 576;
constexpr int kMaxWriteBytes = 8192; // OUT.H contract, including native 7.1 blocks.

void CopyText(wchar_t* target, std::size_t capacity, const wchar_t* text) noexcept {
    if (!target || capacity == 0) return;
    wcsncpy_s(target, capacity, text ? text : L"", _TRUNCATE);
}

TrackInfo FileInfo(const wchar_t* path) {
    if (!path || !*path) {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        return current_info;
    }
    Decoder metadata;
    if (!metadata.Open(path)) return {};
    return metadata.Info();
}

bool WaitForStop(DWORD milliseconds) noexcept {
    return WaitForSingleObject(stop_event, milliseconds) != WAIT_TIMEOUT;
}

unsigned __stdcall DecodeThread(void*) noexcept {
    // Like the stock in_wave/in_vorbis plugins, pass the real channel count
    // through Winamp's DSP and visualization callbacks. DSP may double PCM.
    std::array<std::int16_t, kBlockFrames * kMaxChannels * 2> pcm{};
    const int channels = decoder->OutputChannels();
    const int bytes_per_frame = channels * sizeof(std::int16_t);
    auto* out = module.outMod;
    int pending_bytes = 0, pending_offset = 0;
    int pending_time = 0;
    bool eof = false;
    bool notified = false;
    while (!WaitForStop(0)) {
        const int requested = seek_ms.exchange(-1);
        if (requested >= 0) {
            if (decoder->SeekMs(requested)) {
                pending_bytes = pending_offset = 0;
                eof = false;
                notified = false;
                const int actual = static_cast<int>(decoder->PositionFrames() / (kSampleRate / 1000));
                out->Flush(actual);
                decoded_ms.store(actual);
            } else {
                OutputDebugStringW(L"in_opus: seek failed.\n");
            }
        }
        if (paused.load()) {
            if (WaitForStop(10)) break;
            continue;
        }
        if (pending_bytes > 0) {
            // Drain a block in frame-aligned chunks. Never assume the output
            // buffer can hold an entire 576-frame 7.1 block (9216 bytes).
            int chunk = std::min({out->CanWrite(), pending_bytes, kMaxWriteBytes});
            chunk -= chunk % bytes_per_frame;
            if (chunk > 0 && out->Write(reinterpret_cast<char*>(pcm.data()) + pending_offset, chunk) == 0) {
                pending_offset += chunk;
                pending_bytes -= chunk;
                if (pending_bytes == 0) decoded_ms.store(pending_time);
            } else if (WaitForStop(5)) {
                break;
            }
            continue;
        }
        if (eof) {
            out->CanWrite(); // Give output plugins a chance to update their buffers.
            if (!notified && !out->IsPlaying()) {
                if (module.hMainWindow) PostMessageW(module.hMainWindow, winamp::kEndOfFile, 0, 0);
                notified = true;
            }
            // Stay alive until Stop, so a seek at EOF can still restart decoding.
            if (WaitForStop(10)) break;
            continue;
        }
        if (out->CanWrite() < bytes_per_frame) {
            if (WaitForStop(5)) break;
            continue;
        }
        const int timestamp = decoded_ms.load();
        const int frames = decoder->ReadFrames(pcm.data(), kBlockFrames);
        if (frames <= 0) {
            if (frames < 0) {
                OutputDebugStringW(L"in_opus: decoding stopped: ");
                OutputDebugStringW(ErrorText(frames));
                OutputDebugStringW(L"\n");
            }
            eof = true;
            continue;
        }
        // Both visualization callbacks expect 576 frames, even for a short tail.
        std::fill(pcm.begin() + frames * channels, pcm.begin() + kBlockFrames * channels, std::int16_t{0});
        if (module.SAAddPCMData) module.SAAddPCMData(pcm.data(), channels, 16, timestamp);
        if (module.VSAAddPCMData) module.VSAAddPCMData(pcm.data(), channels, 16, timestamp);

        int output_frames = frames;
        if (module.dsp_isactive && module.dsp_dosamples && module.dsp_isactive()) {
            output_frames = module.dsp_dosamples(pcm.data(), frames, 16, channels, kSampleRate);
            if (output_frames < 0 || output_frames > frames * 2) {
                OutputDebugStringW(L"in_opus: DSP returned an invalid sample count.\n");
                eof = true;
                continue;
            }
        }
        pending_time = static_cast<int>(std::min<std::int64_t>(
            decoder->PositionFrames() / (kSampleRate / 1000), INT_MAX));
        pending_offset = 0;
        pending_bytes = output_frames * bytes_per_frame;
        if (pending_bytes == 0) decoded_ms.store(pending_time);
    }
    return 0;
}

void __cdecl Stop() {
    if (decode_thread) {
        SetEvent(stop_event);
        WaitForSingleObject(decode_thread, INFINITE);
        CloseHandle(std::exchange(decode_thread, nullptr));
    }
    if (stop_event) CloseHandle(std::exchange(stop_event, nullptr));
    if (output_open) {
        module.outMod->Close();
        output_open = false;
    }
    if (visualization_open) {
        if (module.SAVSADeInit) module.SAVSADeInit();
        visualization_open = false;
    }
    decoder.reset();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        active_channels = 0;
        current_path.clear();
        current_output_note.clear();
    }
    paused.store(false);
    seek_ms.store(-1);
    decoded_ms.store(0);
    length_ms.store(0);
}

int __cdecl Play(const wchar_t* path) {
    Stop();
    try {
        auto* out = module.outMod;
        if (!out || !out->Open || !out->Close || !out->Write || !out->CanWrite ||
            !out->IsPlaying || !out->Pause || !out->Flush ||
            !out->GetOutputTime || !out->GetWrittenTime) return 1;
        auto next_decoder = std::make_unique<Decoder>();
        int error = 0;
        if (!next_decoder->Open(path, &error)) return error == OP_EREAD ? -1 : 1;
        auto info = next_decoder->Info();
        std::wstring note;
        if (output_mode.load() == OutputMode::SourceChannels) {
            if (!next_decoder->SetOutputMode(OutputMode::SourceChannels))
                note = L"Stereo fallback: the channel layout changes within the file or is unsupported.";
        }
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) return 1;
        int channels = next_decoder->OutputChannels();
        int latency = out->Open(kSampleRate, channels, 16, 0, 0);
        if (latency < 0 && channels != kStereoChannels) {
            // A failed Open does not acquire output ownership. Retry once with
            // libopusfile's real stereo downmix, never reinterpret native PCM.
            next_decoder->SetOutputMode(OutputMode::Stereo);
            channels = kStereoChannels;
            latency = out->Open(kSampleRate, channels, 16, 0, 0);
            note = L"Stereo fallback: the output plugin rejected the source channel count.";
        }
        if (latency < 0) {
            Stop();
            return 1;
        }
        output_open = true;
        decoder = std::move(next_decoder);
        length_ms.store(info.length_ms);
        if (note.empty()) {
            note = output_mode.load() == OutputMode::SourceChannels
                ? L"Source channels. Speaker routing depends on the output plugin."
                : L"Stereo mode: multichannel recordings are downmixed to 2 channels.";
        }
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            current_info = std::move(info);
            current_path = path;
            current_output_note = std::move(note);
            active_channels = channels;
        }
        if (module.SAVSAInit) {
            module.SAVSAInit(latency, kSampleRate);
            visualization_open = true;
        }
        if (module.VSASetInfo) module.VSASetInfo(kSampleRate, channels);
        if (module.SetInfo) module.SetInfo(BitrateKbps(decoder->Info().bitrate), 48, channels, 1);
        if (out->SetVolume) out->SetVolume(volume);
        if (out->SetPan) out->SetPan(pan);
        decode_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, DecodeThread, nullptr, 0, nullptr));
        if (!decode_thread) {
            Stop();
            return 1;
        }
        return 0;
    } catch (...) {
        Stop();
        return 1; // Never unwind a C++ exception through the host's C ABI.
    }
}

void __cdecl Pause() {
    paused.store(true);
    if (output_open) module.outMod->Pause(1);
}
void __cdecl UnPause() {
    if (output_open) module.outMod->Pause(0);
    paused.store(false);
}
int __cdecl IsPaused() { return paused.load() ? 1 : 0; }
int __cdecl GetLength() { return length_ms.load(); }
int __cdecl GetOutputTime() {
    if (!output_open) return 0;
    // Decode time is in the source timeline; output clocks account for buffering/DSP.
    const auto time = static_cast<std::int64_t>(decoded_ms.load()) +
        module.outMod->GetOutputTime() - module.outMod->GetWrittenTime();
    return static_cast<int>(std::clamp<std::int64_t>(time, 0, length_ms.load()));
}
void __cdecl SetOutputTime(int time) {
    if (output_open) seek_ms.store(std::clamp(time, 0, length_ms.load()));
}
void __cdecl SetVolume(int value) {
    volume = value;
    if (output_open && module.outMod->SetVolume) module.outMod->SetVolume(value);
}
void __cdecl SetPan(int value) {
    pan = value;
    if (output_open && module.outMod->SetPan) module.outMod->SetPan(value);
}
void __cdecl GetFileInfo(const wchar_t* path, wchar_t* title, int* duration) {
    if (title) title[0] = L'\0';
    if (duration) *duration = -1;
    try {
        const auto info = FileInfo(path);
        if (info.total_frames <= 0 && info.display_title.empty()) return;
        CopyText(title, winamp::kTitleCapacity, info.display_title.c_str());
        if (duration) *duration = info.length_ms;
    } catch (...) { }
}
int __cdecl IsOurFile(const wchar_t* path) {
    if (!path || wcsstr(path, L"://")) return 0; // No HTTP or other URL support.
    const auto* extension = wcsrchr(path, L'.');
    if (!extension) return 0;
    if (_wcsicmp(extension, L".opus") == 0) return 1;
    if (_wcsicmp(extension, L".ogg") != 0 && _wcsicmp(extension, L".oga") != 0) return 0;
    // IsOurFile runs before extension matching. Do not globally register .ogg/.oga:
    // that would make Winamp route Vorbis to us when every content probe returns 0.
    return HasOpusContent(path) ? 1 : 0;
}
int __cdecl InfoBox(const wchar_t* path, HWND parent) {
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) {
            MessageBoxW(parent, L"Could not open this Ogg Opus file.",
                L"Opus File Information", MB_OK | MB_ICONERROR);
        } else {
            int channels = kStereoChannels;
            std::wstring note;
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                if (active_channels > 0 && (!path || !*path || _wcsicmp(path, current_path.c_str()) == 0)) {
                    channels = active_channels;
                    note = current_output_note;
                }
            }
            if (note.empty()) {
                if (output_mode.load() == OutputMode::SourceChannels && info.native_channels > 0)
                    channels = info.native_channels;
                note = L"Planned format; output support is checked when playback starts.";
                if (output_mode.load() == OutputMode::SourceChannels && info.native_channels == 0)
                    note = L"Stereo fallback: the channel layout changes within the file or is unsupported.";
            }
            ShowFileInfoDialog(module.hDllInstance, parent, path, info, channels, note);
        }
    } catch (...) { }
    return 1; // INFOBOX_UNCHANGED: metadata editing is not implemented.
}
void __cdecl About(HWND parent) {
    MessageBoxW(parent, L"Winamp Opus Input 1.2.5 (32-bit)\n\n"
        L"libopus 1.6.1 / libopusfile 0.12 / libogg 1.3.6\n"
        L"Local Ogg Opus files (.opus/.ogg/.oga), Unicode paths, seeking, DSP and visualization.\n"
        L"Stereo or original 1-8 channel PCM output.\n"
        L"Audio libraries are linked statically; the C/C++ runtime is loaded from DLLs.",
        L"About Winamp Opus Input", MB_OK | MB_ICONINFORMATION);
}
void __cdecl Config(HWND parent) {
    try {
        std::wstring status = L"Playback is not currently running.";
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            if (active_channels > 0) status = OutputFormatText(active_channels) + L"\n" + current_output_note;
        }
        output_mode.store(ShowOutputConfigDialog(module.hDllInstance, parent, output_mode.load(), status));
    } catch (...) { }
}
void __cdecl Init() { output_mode.store(LoadOutputMode()); }
void __cdecl Quit() { Stop(); }
void __cdecl EQSet(int, char[10], int) { } // Winamp handles its native EQ.

winamp::InModule MakeModule() noexcept {
    static char description[] = "Winamp Opus Input 1.2.5 (x86)";
    static char extensions[] = "opus\0Ogg Opus Audio (*.opus)\0";
    winamp::InModule result{};
    result.version = winamp::kInputVersion;
    result.description = description;
    result.FileExtensions = extensions;
    result.is_seekable = 1;
    result.UsesOutputPlug = winamp::kUsesOutputPlugin;
    result.Config = Config;
    result.About = About;
    result.Init = Init;
    result.Quit = Quit;
    result.GetFileInfo = GetFileInfo;
    result.InfoBox = InfoBox;
    result.IsOurFile = IsOurFile;
    result.Play = Play;
    result.Pause = Pause;
    result.UnPause = UnPause;
    result.IsPaused = IsPaused;
    result.Stop = Stop;
    result.GetLength = GetLength;
    result.GetOutputTime = GetOutputTime;
    result.SetOutputTime = SetOutputTime;
    result.SetVolume = SetVolume;
    result.SetPan = SetPan;
    result.EQSet = EQSet;
    return result;
}
const bool initialized = [] { module = MakeModule(); return true; }();
} // namespace

extern "C" winamp::InModule* __cdecl winampGetInModule2() {
    return &module;
}

extern "C" int __cdecl winampGetExtendedFileInfoW(const wchar_t* path,
    const char* key, wchar_t* result, std::size_t capacity) {
    if (!key || !result || capacity == 0) return 0;
    result[0] = L'\0';
    // Reject unsupported fields before opening the file. Classify only once.
    enum class Field { Title, Artist, Album, Length, Bitrate, SampleRate,
        Channels, Type, Lossless, Family };
    Field field;
    if (_stricmp(key, "title") == 0) field = Field::Title;
    else if (_stricmp(key, "artist") == 0) field = Field::Artist;
    else if (_stricmp(key, "album") == 0) field = Field::Album;
    else if (_stricmp(key, "length") == 0) field = Field::Length;
    else if (_stricmp(key, "bitrate") == 0) field = Field::Bitrate;
    else if (_stricmp(key, "samplerate") == 0) field = Field::SampleRate;
    else if (_stricmp(key, "channels") == 0) field = Field::Channels;
    else if (_stricmp(key, "type") == 0) field = Field::Type;
    else if (_stricmp(key, "lossless") == 0) field = Field::Lossless;
    else if (_stricmp(key, "family") == 0) field = Field::Family;
    else return 0;
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) return 0;
        std::wstring value;
        switch (field) {
        case Field::Title: value = info.title; break;
        case Field::Artist: value = info.artist; break;
        case Field::Album: value = info.album; break;
        case Field::Length: value = std::to_wstring(info.length_ms); break;
        case Field::Bitrate: value = std::to_wstring(BitrateKbps(info.bitrate)); break;
        case Field::SampleRate: value = L"48000"; break;
        case Field::Channels: value = std::to_wstring(info.source_channels); break;
        case Field::Type:
        case Field::Lossless: value = L"0"; break;
        case Field::Family: value = L"Ogg Opus"; break;
        }
        CopyText(result, capacity, value.c_str());
        return 1;
    } catch (...) {
        return 0;
    }
}
