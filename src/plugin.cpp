#include "opus_decoder.hpp"
#include "pcm_pipeline.hpp"
#include "file_info_dialog.hpp"
#include "metadata.hpp"
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
std::unique_ptr<PcmPipeline> pipeline;
HANDLE stop_event = nullptr, decode_thread = nullptr;
bool output_open = false, visualization_open = false;
std::atomic<bool> paused{false};
std::atomic<int> seek_ms{-1}, decoded_ms{0}, length_ms{0};
int volume = -666, pan = 0;
std::mutex metadata_mutex;
TrackInfo current_info;
std::wstring current_path, current_output_note;
int active_channels = 0;
PlaybackSettings configured_settings, active_settings;
bool active_shuffle = false; // Immutable while the decode thread is alive.
struct MetadataCache {
    std::wstring path;
    TrackInfo info;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    bool has_attributes = false;
    ULONGLONG expires = 0;
} metadata_cache;
struct ProbeCache { std::wstring url; bool opus = false; ULONGLONG expires = 0; };
std::array<ProbeCache, 16> probe_cache;
std::size_t next_probe = 0;
constexpr int kBlockFrames = 576, kMaxWriteBytes = 8192;
constexpr UINT kWinampIpc = WM_USER;
constexpr int kGetShuffle = 250, kUpdateTitle = 243;

void CopyText(wchar_t* target, std::size_t capacity, const wchar_t* text) noexcept {
    if (target && capacity) wcsncpy_s(target, capacity, text ? text : L"", _TRUNCATE);
}
PlaybackSettings Settings() {
    std::lock_guard<std::mutex> lock(metadata_mutex); return configured_settings;
}
TrackInfo FileInfo(const wchar_t* path) {
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (!path || !*path || (active_channels && _wcsicmp(path, current_path.c_str()) == 0)) return current_info;
    }
    if (IsHttpUrl(path)) {
        // Playlist metadata queries must not open a fresh radio connection for
        // every field. The playing stream supplies its actual tags/station data.
        TrackInfo info; info.radio = true; info.seekable = false;
        info.total_frames = info.length_ms = -1; info.display_title = StreamUrl(path);
        return info;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const bool valid = GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) != FALSE;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        const auto& cache = metadata_cache;
        if (_wcsicmp(cache.path.c_str(), path) == 0 &&
            ((valid && cache.has_attributes && attributes.nFileSizeLow == cache.attributes.nFileSizeLow &&
            attributes.nFileSizeHigh == cache.attributes.nFileSizeHigh &&
            CompareFileTime(&attributes.ftLastWriteTime, &cache.attributes.ftLastWriteTime) == 0) ||
            (!valid && !cache.has_attributes && GetTickCount64() < cache.expires))) return cache.info;
    }
    Decoder metadata;
    TrackInfo info;
    if (metadata.Open(path)) info = metadata.Info();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        metadata_cache = {path, info, attributes, valid, GetTickCount64() + 1000};
    }
    return info;
}
bool WaitForStop(DWORD ms) noexcept { return WaitForSingleObject(stop_event, ms) != WAIT_TIMEOUT; }
int Timeline(std::int64_t frames) noexcept { return static_cast<int>(std::clamp<std::int64_t>(frames / 48, 0, INT_MAX)); }

unsigned DecodeLoop() {
    // Aligned storage for the widest PCM format and the SDK's 2x DSP expansion.
    std::array<std::uint32_t, kBlockFrames * kMaxChannels * 2> storage{};
    std::array<std::int16_t, kBlockFrames * kMaxChannels> visualization{};
    auto* pcm = reinterpret_cast<unsigned char*>(storage.data());
    const int channels = decoder->OutputChannels();
    const auto settings = active_settings;
    const int bytes_per_sample = settings.bits / 8, bytes_per_frame = channels * bytes_per_sample;
    auto* out = module.outMod;
    int pending_bytes = 0, pending_offset = 0, pending_time = 0;
    bool eof = false, notified = false;
    auto revision = decoder->MetadataRevision();
    ULONGLONG next_bitrate = GetTickCount64() + 250;
    while (!WaitForStop(0)) {
        const int requested = seek_ms.exchange(-1);
        if (requested >= 0 && decoder->SeekMs(requested)) {
            pipeline->ResetAfterSeek();
            pending_bytes = pending_offset = 0; eof = notified = false;
            const int actual = Timeline(pipeline->PositionFrames());
            out->Flush(actual); decoded_ms.store(actual);
        }
        if (paused.load()) { if (WaitForStop(10)) break; continue; }
        if (pending_bytes > 0) {
            int chunk = std::min({out->CanWrite(), pending_bytes, kMaxWriteBytes});
            chunk -= chunk % bytes_per_frame;
            if (chunk > 0 && out->Write(reinterpret_cast<char*>(pcm) + pending_offset, chunk) == 0) {
                pending_offset += chunk; pending_bytes -= chunk;
                if (!pending_bytes) decoded_ms.store(pending_time);
            } else if (WaitForStop(5)) break;
            continue;
        }
        if (eof) {
            out->CanWrite();
            if (!notified && !out->IsPlaying()) {
                if (module.hMainWindow) PostMessageW(module.hMainWindow, winamp::kEndOfFile, 0, 0);
                notified = true;
            }
            if (WaitForStop(10)) break;
            continue;
        }
        if (out->CanWrite() < bytes_per_frame) { if (WaitForStop(5)) break; continue; }
        const int timestamp = decoded_ms.load();
        const int frames = pipeline->Read(pcm, kBlockFrames);
        if (frames <= 0) {
            if (frames < 0 && !WaitForStop(0)) { OutputDebugStringW(L"in_opus: "); OutputDebugStringW(ErrorText(frames)); OutputDebugStringW(L"\n"); }
            eof = true; continue;
        }
        std::memset(pcm + frames * bytes_per_frame, settings.bits == 8 ? 128 : 0,
            (kBlockFrames - frames) * bytes_per_frame);
        // Always give visualizers 16-bit data; the DSP/output retain the chosen
        // resolution and all source channels. Never quantize the audio for visuals.
        const void* visual = pcm;
        if (settings.bits != 16) {
            for (int i = 0; i < kBlockFrames * channels; ++i) {
                const auto* p = pcm + i * bytes_per_sample;
                visualization[i] = settings.bits == 8 ? static_cast<std::int16_t>((static_cast<int>(*p) - 128) * 256)
                    : static_cast<std::int16_t>(p[bytes_per_sample - 2] | (p[bytes_per_sample - 1] << 8));
            }
            visual = visualization.data();
        }
        if (module.SAAddPCMData) module.SAAddPCMData(const_cast<void*>(visual), channels, 16, timestamp);
        if (module.VSAAddPCMData) module.VSAAddPCMData(const_cast<void*>(visual), channels, 16, timestamp);
        int output_frames = frames;
        if (module.dsp_isactive && module.dsp_dosamples && module.dsp_isactive()) {
            output_frames = module.dsp_dosamples(reinterpret_cast<short*>(pcm), frames, settings.bits, channels, settings.sample_rate);
            if (output_frames < 0 || output_frames > frames * 2) { OutputDebugStringW(L"in_opus: invalid DSP sample count.\n"); eof = true; continue; }
        }
        pending_time = Timeline(pipeline->PositionFrames());
        pending_offset = 0; pending_bytes = output_frames * bytes_per_frame;
        if (!pending_bytes) decoded_ms.store(pending_time);
        if (revision != decoder->MetadataRevision()) {
            revision = decoder->MetadataRevision();
            decoder->SetGain(settings, active_shuffle);
            { std::lock_guard<std::mutex> lock(metadata_mutex); current_info = decoder->Info(); }
            if (module.hMainWindow) PostMessageW(module.hMainWindow, kWinampIpc, 0, kUpdateTitle);
        }
        if (settings.instant_bitrate && GetTickCount64() >= next_bitrate) {
            const int bitrate = decoder->InstantBitrate();
            if (bitrate > 0 && module.SetInfo) module.SetInfo(BitrateKbps(bitrate), settings.sample_rate / 1000, channels, 1);
            next_bitrate = GetTickCount64() + 250;
        }
    }
    return 0;
}
unsigned __stdcall DecodeThread(void*) noexcept {
    try { return DecodeLoop(); }
    catch (...) {
        OutputDebugStringW(L"in_opus: decode thread failed.\n");
        if (module.hMainWindow) PostMessageW(module.hMainWindow, winamp::kEndOfFile, 0, 0);
        return 1;
    }
}
void __cdecl Stop() {
    if (decode_thread) {
        SetEvent(stop_event);
        if (decoder) decoder->Cancel(); // Interrupt an outstanding network read.
        WaitForSingleObject(decode_thread, INFINITE);
        CloseHandle(std::exchange(decode_thread, nullptr));
    }
    if (stop_event) CloseHandle(std::exchange(stop_event, nullptr));
    if (output_open) { module.outMod->Close(); output_open = false; }
    if (visualization_open) { if (module.SAVSADeInit) module.SAVSADeInit(); visualization_open = false; }
    pipeline.reset(); decoder.reset();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        active_channels = 0; current_path.clear(); current_output_note.clear();
    }
    paused.store(false); seek_ms.store(-1); decoded_ms.store(0); length_ms.store(0);
    module.is_seekable = 1;
}
int __cdecl Play(const wchar_t* path) {
    Stop();
    try {
        auto* out = module.outMod;
        if (!out || !out->Open || !out->Close || !out->Write || !out->CanWrite || !out->IsPlaying ||
            !out->Pause || !out->Flush || !out->GetOutputTime || !out->GetWrittenTime) return 1;
        const auto settings = Settings();
        auto next = std::make_unique<Decoder>();
        int error = 0;
        if (!next->Open(path, &error)) return error == OP_EREAD && !IsHttpUrl(path) ? -1 : 1;
        auto info = next->Info();
        const bool shuffle = module.hMainWindow && settings.gain_mode == GainMode::Auto &&
            SendMessageW(module.hMainWindow, kWinampIpc, 0, kGetShuffle) != 0;
        next->SetGain(settings, shuffle);
        std::wstring note;
        if (settings.output_mode == OutputMode::SourceChannels && !next->SetOutputMode(OutputMode::SourceChannels))
            note = info.radio ? L"Live radio: stereo output supports changing stream layouts."
                : L"Stereo fallback: the channel layout changes within the file or is unsupported.";
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) return 1;
        int channels = next->OutputChannels();
        int latency = out->Open(settings.sample_rate, channels, settings.bits, 0, 0);
        if (latency < 0 && channels != kStereoChannels) {
            next->SetOutputMode(OutputMode::Stereo); channels = kStereoChannels;
            latency = out->Open(settings.sample_rate, channels, settings.bits, 0, 0);
            note = L"Stereo fallback: the output plugin rejected the source channel count.";
        }
        if (latency < 0) { Stop(); return 1; }
        output_open = true;
        decoder = std::move(next);
        pipeline = std::make_unique<PcmPipeline>(*decoder, settings);
        if (!pipeline->Ready()) { Stop(); return 1; }
        length_ms.store(info.length_ms); module.is_seekable = info.seekable ? 1 : 0;
        if (note.empty()) note = settings.output_mode == OutputMode::SourceChannels
            ? L"Source channels. Speaker routing depends on the output plugin."
            : L"Stereo mode: multichannel recordings are downmixed to 2 channels.";
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            current_info = std::move(info); current_path = path; current_output_note = std::move(note);
            active_channels = channels; active_settings = settings; active_shuffle = shuffle;
        }
        if (module.SAVSAInit) { module.SAVSAInit(latency, settings.sample_rate); visualization_open = true; }
        if (module.VSASetInfo) module.VSASetInfo(settings.sample_rate, channels);
        if (module.SetInfo) module.SetInfo(BitrateKbps(decoder->Info().bitrate), settings.sample_rate / 1000, channels, 1);
        if (out->SetVolume) out->SetVolume(volume);
        if (out->SetPan) out->SetPan(pan);
        decode_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, DecodeThread, nullptr, 0, nullptr));
        if (!decode_thread) { Stop(); return 1; }
        return 0;
    } catch (...) { Stop(); return 1; }
}
void __cdecl Pause() { paused.store(true); if (output_open) module.outMod->Pause(1); }
void __cdecl UnPause() { if (output_open) module.outMod->Pause(0); paused.store(false); }
int __cdecl IsPaused() { return paused.load() ? 1 : 0; }
int __cdecl GetLength() { return length_ms.load(); }
int __cdecl GetOutputTime() {
    if (!output_open) return 0;
    const auto time = static_cast<std::int64_t>(decoded_ms.load()) + module.outMod->GetOutputTime() - module.outMod->GetWrittenTime();
    const int length = length_ms.load();
    return static_cast<int>(std::clamp<std::int64_t>(time, 0, length >= 0 ? length : INT_MAX));
}
void __cdecl SetOutputTime(int time) {
    if (output_open && module.is_seekable && length_ms.load() >= 0) seek_ms.store(std::clamp(time, 0, length_ms.load()));
}
void __cdecl SetVolume(int value) { volume = value; if (output_open && module.outMod->SetVolume) module.outMod->SetVolume(value); }
void __cdecl SetPan(int value) { pan = value; if (output_open && module.outMod->SetPan) module.outMod->SetPan(value); }
void __cdecl GetFileInfo(const wchar_t* path, wchar_t* title, int* duration) {
    if (title) *title = L'\0'; if (duration) *duration = -1;
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) return;
        CopyText(title, winamp::kTitleCapacity, info.display_title.c_str());
        if (duration) *duration = info.length_ms;
    } catch (...) { }
}
int __cdecl IsOurFile(const wchar_t* path) {
    if (!path || !*path) return 0;
    try {
        if (IsHttpUrl(path)) {
            const auto path_length = std::wcslen(path);
            if (path_length >= 6 && _wcsicmp(path + path_length - 6, L">.opus") == 0) return 1;
            const auto url = StreamUrl(path);
            auto base = url.substr(0, url.find_first_of(L"?#"));
            const auto dot = base.find_last_of(L'.');
            if (dot != std::wstring::npos && (_wcsicmp(base.c_str() + dot, L".opus") == 0 ||
                _wcsicmp(base.c_str() + dot, L".opu") == 0)) return 1;
            const auto now = GetTickCount64();
            { std::lock_guard<std::mutex> lock(metadata_mutex);
              for (const auto& probe : probe_cache) if (probe.url == url && probe.expires > now) return probe.opus ? 1 : 0; }
            const bool opus = HasOpusContent(path);
            { std::lock_guard<std::mutex> lock(metadata_mutex);
              probe_cache[next_probe++ % probe_cache.size()] = {url, opus, GetTickCount64() + 60000}; }
            return opus ? 1 : 0;
        }
        if (wcsstr(path, L"://")) return 0;
        const auto* extension = wcsrchr(path, L'.');
        if (!extension) return 0;
        if (_wcsicmp(extension, L".opus") == 0 || _wcsicmp(extension, L".opu") == 0) return 1;
        if (_wcsicmp(extension, L".ogg") != 0 && _wcsicmp(extension, L".oga") != 0) return 0;
        // Do not register .ogg/.oga globally: Vorbis belongs to its own plugin.
        return HasOpusContent(path) ? 1 : 0;
    } catch (...) { return 0; }
}
int __cdecl InfoBox(const wchar_t* path, HWND parent) {
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) MessageBoxW(parent, L"Could not open this Ogg Opus file.", L"Opus File Information", MB_OK | MB_ICONERROR);
        else {
            auto settings = Settings();
            int channels = settings.output_mode == OutputMode::SourceChannels && info.native_channels > 0 ? info.native_channels : 2;
            std::wstring note = L"Planned format; output support is checked when playback starts.";
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                if (active_channels > 0 && (!path || !*path || _wcsicmp(path, current_path.c_str()) == 0)) {
                    channels = active_channels; note = current_output_note; settings = active_settings;
                } else if (settings.output_mode == OutputMode::SourceChannels && info.native_channels == 0)
                    note = info.radio ? L"Live radio uses stereo. Start playback to read station information."
                        : L"Stereo fallback: the channel layout changes within the file or is unsupported.";
            }
            ShowFileInfoDialog(module.hDllInstance, parent, path, info, channels, note, settings.sample_rate, settings.bits);
        }
    } catch (...) { }
    return 1;
}
void __cdecl About(HWND parent) {
    MessageBoxW(parent, L"Winamp Opus Input 1.3.0 (32-bit)\n\n"
        L"libopus 1.6.1 / libopusfile 0.12 / libogg 1.3.6 / SpeexDSP 1.2.1\n"
        L"Local Ogg Opus and HTTP(S) radio, Unicode, ReplayGain/R128, seeking, DSP and visualization.\n"
        L"Source 1-8 channels or stereo; 8/16/24/32-bit PCM; optional resampling.\n"
        L"Audio libraries are static; the C/C++ runtime is loaded from DLLs.",
        L"About Winamp Opus Input", MB_OK | MB_ICONINFORMATION);
}
void __cdecl Config(HWND parent) {
    try {
        auto settings = Settings(); std::wstring status = L"Playback is not currently running.";
        { std::lock_guard<std::mutex> lock(metadata_mutex);
          if (active_channels > 0) status = OutputFormatText(active_channels, active_settings.sample_rate, active_settings.bits); }
        settings = ShowPlaybackConfigDialog(module.hDllInstance, parent, settings, status);
        { std::lock_guard<std::mutex> lock(metadata_mutex); configured_settings = settings; }
    } catch (...) { }
}
void __cdecl Init() { std::lock_guard<std::mutex> lock(metadata_mutex); configured_settings = LoadPlaybackSettings(); }
void __cdecl Quit() { Stop(); }
void __cdecl EQSet(int, char[10], int) { }
winamp::InModule MakeModule() noexcept {
    static char description[] = "Winamp Opus Input 1.3.0 (x86)";
    static char extensions[] = "opus;opu\0Ogg Opus Audio (*.opus;*.opu)\0";
    winamp::InModule r{};
    r.version = winamp::kInputVersion; r.description = description; r.FileExtensions = extensions;
    r.is_seekable = 1; r.UsesOutputPlug = winamp::kUsesOutputPlugin;
    r.Config = Config; r.About = About; r.Init = Init; r.Quit = Quit;
    r.GetFileInfo = GetFileInfo; r.InfoBox = InfoBox; r.IsOurFile = IsOurFile; r.Play = Play;
    r.Pause = Pause; r.UnPause = UnPause; r.IsPaused = IsPaused; r.Stop = Stop;
    r.GetLength = GetLength; r.GetOutputTime = GetOutputTime; r.SetOutputTime = SetOutputTime;
    r.SetVolume = SetVolume; r.SetPan = SetPan; r.EQSet = EQSet;
    return r;
}
const bool initialized = [] { module = MakeModule(); return true; }();
}
extern "C" winamp::InModule* __cdecl winampGetInModule2() { return &module; }
extern "C" int __cdecl winampGetExtendedFileInfoW(const wchar_t* path, const char* key, wchar_t* result, int capacity) {
    if (!key || !result || capacity <= 0) return 0;
    result[0] = L'\0';
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) return 0;
        std::wstring value;
        if (!MetadataValue(info, key, value)) return 0;
        CopyText(result, static_cast<std::size_t>(capacity), value.c_str()); return 1;
    } catch (...) { return 0; }
}
extern "C" int __cdecl winampGetExtendedFileInfo(const char* path, const char* key, char* result, int capacity) {
    if (!path || !key || !result || capacity <= 0) return 0;
    result[0] = '\0';
    try {
        const int length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        if (length <= 0) return 0;
        std::wstring wide(length, L'\0'); MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), length);
        const auto info = FileInfo(wide.c_str());
        std::wstring value;
        if (info.display_title.empty() || !MetadataValue(info, key, value)) return 0;
        const int bytes = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return 0;
        std::string ansi(bytes, '\0'); WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, ansi.data(), bytes, nullptr, nullptr);
        strncpy_s(result, static_cast<std::size_t>(capacity), ansi.c_str(), _TRUNCATE); return 1;
    } catch (...) { return 0; }
}
extern "C" int __cdecl winampUseUnifiedFileInfoDlg(const wchar_t* path) {
    try { return Settings().unified_info && path && *path && !wcsstr(path, L"://") && HasOpusContent(path) ? 1 : 0; }
    catch (...) { return 0; }
}
extern "C" HWND __cdecl winampAddUnifiedFileInfoPane(int index, const wchar_t* path, HWND parent, wchar_t* name, int capacity) {
    if (index != 0 || !path || !parent || IsHttpUrl(path)) return nullptr;
    try {
        const auto info = FileInfo(path);
        if (info.display_title.empty()) return nullptr;
        CopyText(name, capacity > 0 ? static_cast<std::size_t>(capacity) : 0, L"Opus (read-only)");
        return CreateUnifiedInfoPane(module.hDllInstance, parent, info);
    } catch (...) { return nullptr; }
}
