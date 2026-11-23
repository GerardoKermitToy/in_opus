#include "opus_decoder.hpp"
#include "pcm_pipeline.hpp"
#include "file_info_dialog.hpp"
#include "metadata.hpp"
#include "output_settings.hpp"
#include "winamp_input.hpp"
#include "host_settings.hpp"
#include "language.hpp"
#include "tag_edit.hpp"
#include "tag_writer.hpp"
#include <charconv>

#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <cwchar>
#include <memory>
#include <filesystem>
#include <vector>
#include <mutex>
#include <string>
#include <utility>

using namespace winamp_opus;
namespace {
winamp::InModule module{};
std::unique_ptr<Decoder> decoder;
std::unique_ptr<PcmPipeline> pipeline;
HANDLE stop_event = nullptr, control_event = nullptr, decode_thread = nullptr;
bool output_open = false, visualization_open = false;
std::atomic<bool> paused{false};
std::atomic<int> seek_ms{-1}, decoded_ms{0}, length_ms{0};
int volume = -666, pan = 0;
std::mutex metadata_mutex;
TrackInfo current_info;
std::wstring current_path, current_output_note;
int active_channels = 0;
PlaybackSettings configured_settings, active_settings;
std::vector<OpusTagLink> edited_links;
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
std::wstring CanonicalPath(const wchar_t* path) {
    if(!path || !*path)return {};
    if(IsHttpUrl(path))return path;
    const DWORD needed=GetFullPathNameW(path,0,nullptr,nullptr);
    if(!needed || needed>32768)return path;
    std::wstring value(needed,L'\0');
    const DWORD used=GetFullPathNameW(path,needed,value.data(),nullptr);
    if(!used || used>=needed)return path;
    value.resize(used);return value;
}
TrackInfo FileInfo(const wchar_t* path) {
    const auto canonical=CanonicalPath(path);
    if(!canonical.empty())path=canonical.c_str();
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
    if (metadata.Open(path, nullptr, Settings())) info = metadata.Info();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        metadata_cache = {path, info, attributes, valid, GetTickCount64() + 1000};
    }
    return info;
}
void ApplyEditedTags(TrackInfo& info) {
    // Called with metadata_mutex held. An edit affects host metadata, not the
    // already-open encoded audio handle and its exact decoder history.
    if (info.current_link < 0 || static_cast<std::size_t>(info.current_link) >= edited_links.size()) return;
    const auto& edited = edited_links[info.current_link];
    if (info.serial && info.serial != edited.serial) return;
    info.tags = edited.tags; info.vendor = edited.vendor;
    auto tag = [&](const char* key) { const auto i = info.tags.find(key); return i == info.tags.end() ? std::wstring{} : i->second; };
    info.title = tag("title"); info.artist = tag("artist"); info.album = tag("album");
    auto gain = [&](const char* key, int& q8) {
        const auto value = tag(key); if (value.empty() || value.size() > 7) return false;
        char text[8]{}; for (std::size_t i=0;i<value.size();++i) { if (value[i]>127) return false; text[i]=static_cast<char>(value[i]); }
        const char* start=text; if(*start=='+')++start;
        const auto parsed=std::from_chars(start,text+value.size(),q8);
        return parsed.ec==std::errc{} && parsed.ptr==text+value.size() && q8>=-32768 && q8<=32767;
    };
    info.has_track_gain=gain("r128_track_gain",info.track_gain_q8);
    info.has_album_gain=gain("r128_album_gain",info.album_gain_q8);
    const auto base=info.title.empty()?std::filesystem::path(current_path).stem().wstring():info.title;
    info.display_title=info.artist.empty()?base:info.artist+L" - "+base;
    if(!info.station.title.empty())info.display_title=info.station.title;
}
void InvalidateMetadata(const wchar_t* path) {
    try {
        std::vector<OpusTagLink> links; std::wstring error;
        bool playing=false;
        { std::lock_guard<std::mutex> lock(metadata_mutex); playing=path && active_channels && _wcsicmp(path,current_path.c_str())==0; }
        const bool read = playing && ReadOpusTagLinks(path, links, error);
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            metadata_cache = {};
            if (read && active_channels && _wcsicmp(path,current_path.c_str())==0) {
                const auto tags=current_info.tags; const auto title=current_info.display_title;
                edited_links=std::move(links); ApplyEditedTags(current_info);
                notify = current_info.tags != tags || current_info.display_title != title;
            } else notify = path && *path; // Confirmed explicit edits of a non-playing file.
        }
        if(notify && module.hMainWindow)PostMessageW(module.hMainWindow,kWinampIpc,0,kUpdateTitle);
    } catch (...) { std::lock_guard<std::mutex> lock(metadata_mutex); metadata_cache = {}; }
}
bool WaitForStop(DWORD ms) noexcept { return WaitForSingleObject(stop_event, ms) != WAIT_TIMEOUT; }
void WaitForControl() noexcept {
    const HANDLE events[]{stop_event, control_event};
    WaitForMultipleObjects(2, events, FALSE, INFINITE);
}
int Timeline(std::int64_t frames) noexcept { return static_cast<int>(std::clamp<std::int64_t>(frames / 48, 0, INT_MAX)); }

unsigned DecodeLoop() {
    // Aligned storage for the widest PCM format and the SDK's 2x DSP expansion.
    std::array<std::uint32_t, kBlockFrames * kMaxChannels * 2> storage{};
    std::array<std::int16_t, kBlockFrames * kMaxChannels> visualization{};
    auto* pcm = reinterpret_cast<unsigned char*>(storage.data());
    const int channels = pipeline->Channels();
    const auto settings = active_settings;
    const int bytes_per_sample = settings.bits / 8, bytes_per_frame = channels * bytes_per_sample;
    auto* out = module.outMod;
    int pending_bytes = 0, pending_offset = 0, pending_time = 0;
    bool eof = false, notified = false;
    int reconnects = 0, connection_base_ms = 0;
    const auto playback_path = current_path;
    auto revision = decoder->MetadataRevision();
    int displayed_bitrate = BitrateKbps(decoder->Info().bitrate);
    ULONGLONG next_bitrate = GetTickCount64() + 250;
    auto publish_metadata = [&] {
        if (revision == decoder->MetadataRevision()) return;
        revision = decoder->MetadataRevision();
        // Gain is already updated at the codec boundary before its first frame.
        // Publishing a host snapshot must not recompute/apply it a second time.
        const auto& info = decoder->Info();
        bool title_changed;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            // ATF can use any tag, not just our default display title.
            // Decoder diagnostics and seek positions are not playlist metadata.
            TrackInfo published=info;
            ApplyEditedTags(published);
            title_changed = current_info.display_title != published.display_title || current_info.tags != published.tags;
            current_info = std::move(published);
        }
        if (title_changed && module.hMainWindow) PostMessageW(module.hMainWindow, kWinampIpc, 0, kUpdateTitle);
    };
    while (!WaitForStop(0)) {
        const int requested = seek_ms.exchange(-1);
        if (requested >= 0 && decoder->SeekMs(requested)) {
            pipeline->ResetAfterSeek();
            pending_bytes = pending_offset = 0; eof = notified = false;
            const int actual = Timeline(pipeline->PositionFrames()) + connection_base_ms;
            out->Flush(actual); decoded_ms.store(actual);
            publish_metadata();
        }
        if (paused.load()) { WaitForControl(); continue; }
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
            if (notified) { WaitForControl(); continue; }
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
        publish_metadata();
        if (frames <= 0) {
            { std::lock_guard<std::mutex> lock(metadata_mutex); current_info = decoder->Info(); ApplyEditedTags(current_info); }
            if (frames < 0 && !WaitForStop(0)) {
                OutputDebugStringW(L"in_opus: ");
                OutputDebugStringW(decoder->LastError().empty() ? ErrorText(frames) : decoder->LastError().c_str());
                OutputDebugStringW(L"\n");
            }
            if (decoder->Info().radio && reconnects < settings.reconnect_attempts) {
                bool reopened=false;
                while(reconnects < settings.reconnect_attempts && !WaitForStop(0)) {
                    ++reconnects;
                    if(WaitForStop(static_cast<DWORD>(settings.reconnect_delay_ms)))break;
                    if(!decoder->Open(playback_path.c_str(),nullptr,settings))continue;
                    decoder->SetGain(settings,active_shuffle);
                    if(settings.output_mode!=OutputMode::Stereo)decoder->SetOutputMode(OutputMode::SourceChannels);
                    auto resumed=std::make_unique<PcmPipeline>(*decoder,settings);
                    if(!resumed->Ready() || resumed->Channels()!=channels || resumed->Rate()!=settings.sample_rate) {
                        // A new source must fit the established output; only the
                        // internal decoder may fall back to stereo, never reopen out.
                        decoder->SetOutputMode(OutputMode::Stereo);
                        resumed=std::make_unique<PcmPipeline>(*decoder,settings);
                    }
                    if(!resumed->Ready() || resumed->Channels()!=channels || resumed->Rate()!=settings.sample_rate)continue;
                    connection_base_ms=decoded_ms.load();
                    pipeline=std::move(resumed); revision=~decoder->MetadataRevision();
                    publish_metadata(); reopened=true;break;
                }
                if(reopened)continue;
            }
            eof = true; continue;
        }
        if (module.SAAddPCMData || module.VSAAddPCMData) {
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
        }
        int output_frames = frames;
        if (module.dsp_isactive && module.dsp_dosamples && module.dsp_isactive()) {
            output_frames = module.dsp_dosamples(reinterpret_cast<short*>(pcm), frames, settings.bits, channels, settings.sample_rate);
            if (output_frames < 0 || output_frames > frames * 2) { OutputDebugStringW(L"in_opus: invalid DSP sample count.\n"); eof = true; continue; }
        }
        pending_time = Timeline(pipeline->PositionFrames()) + connection_base_ms;
        pending_offset = 0; pending_bytes = output_frames * bytes_per_frame;
        if (!pending_bytes) decoded_ms.store(pending_time);
        if (GetTickCount64() >= next_bitrate) {
            {
                // Only packet diagnostics vary between real metadata events.
                const auto& info = decoder->Info();
                std::lock_guard<std::mutex> lock(metadata_mutex);
                current_info.bandwidth_hz = info.bandwidth_hz;
                current_info.packet_duration_us = info.packet_duration_us;
                current_info.pitch48 = info.pitch48; current_info.final_range = info.final_range;
                current_info.concealed_frames48 = info.concealed_frames48;
                current_info.fec_frames48 = info.fec_frames48; current_info.dred_frames48 = info.dred_frames48;
                current_info.qext = info.qext;
                if (current_info.decoder_note != info.decoder_note) current_info.decoder_note = info.decoder_note;
            }
            if (settings.instant_bitrate) {
                const int bitrate = BitrateKbps(decoder->InstantBitrate());
                if (bitrate > 0 && bitrate != displayed_bitrate && module.SetInfo) {
                    // In Winamp, supplying both bitrate and sample rate requests
                    // a title refresh. Publish the format once in Play(); -1
                    // preserves unchanged fields during bitrate-only updates.
                    module.SetInfo(bitrate, -1, -1, 1);
                    displayed_bitrate = bitrate;
                }
            }
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
    if (control_event) CloseHandle(std::exchange(control_event, nullptr));
    if (output_open) { module.outMod->Close(); output_open = false; }
    if (visualization_open) { if (module.SAVSADeInit) module.SAVSADeInit(); visualization_open = false; }
    pipeline.reset(); decoder.reset();
    {
        std::lock_guard<std::mutex> lock(metadata_mutex);
        active_channels = 0; current_path.clear(); current_output_note.clear(); edited_links.clear();
    }
    paused.store(false); seek_ms.store(-1); decoded_ms.store(0); length_ms.store(0);
    module.is_seekable = 1;
}
int __cdecl Play(const wchar_t* path) {
    Stop();
    try {
        const auto canonical=CanonicalPath(path); path=canonical.c_str();
        auto* out = module.outMod;
        if (!out || !out->Open || !out->Close || !out->Write || !out->CanWrite || !out->IsPlaying ||
            !out->Pause || !out->Flush || !out->GetOutputTime || !out->GetWrittenTime) return 1;
        auto local_settings=Settings();
        // Proxy configuration has no bearing on a local file; avoid querying
        // that optional host service when no network transport will be used.
        if(!IsHttpUrl(path))local_settings.use_host_proxy=false;
        auto settings = ResolveHostSettings(module.hMainWindow, std::move(local_settings));
        auto next = std::make_unique<Decoder>();
        int error = 0;
        if (!next->Open(path, &error, settings)) return error == OP_EREAD && !IsHttpUrl(path) ? -1 : 1;
        if (!settings.sample_rate) settings.sample_rate = next->SampleRate();
        auto info = next->Info();
        const bool shuffle = module.hMainWindow && settings.gain_mode == GainMode::Auto &&
            SendMessageW(module.hMainWindow, kWinampIpc, 0, kGetShuffle) != 0;
        next->SetGain(settings, shuffle);
        std::wstring note;
        if (settings.output_mode != OutputMode::Stereo && !next->SetOutputMode(OutputMode::SourceChannels))
            note = info.radio ? L"Live radio: stereo output supports changing stream layouts."
                : L"Stereo fallback: the channel layout changes within the file or is unsupported.";
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        control_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stop_event || !control_event) { Stop(); return 1; }
        auto next_pipeline=std::make_unique<PcmPipeline>(*next,settings);
        if(!next_pipeline->Ready()){Stop();return 1;}
        int channels = next_pipeline->Channels();
        int latency = out->Open(settings.sample_rate, channels, settings.bits, 0, 0);
        if (latency < 0 && channels != kStereoChannels) {
            next->SetOutputMode(OutputMode::Stereo); settings.output_mode=OutputMode::Stereo;
            next_pipeline=std::make_unique<PcmPipeline>(*next,settings);
            if(!next_pipeline->Ready()){Stop();return 1;}
            channels = next_pipeline->Channels();
            latency = out->Open(settings.sample_rate, channels, settings.bits, 0, 0);
            note = L"Stereo fallback: the output plugin rejected the source channel count.";
        }
        if (latency < 0) { Stop(); return 1; }
        output_open = true;
        decoder = std::move(next);
        pipeline = std::move(next_pipeline);
        if (!pipeline->Ready()) { Stop(); return 1; }
        length_ms.store(info.length_ms); module.is_seekable = info.seekable ? 1 : 0;
        if (note.empty()) note = settings.output_mode == OutputMode::SourceChannels
            ? L"Source channels. Speaker routing depends on the output plugin."
            : settings.output_mode==OutputMode::Stereo ? L"Stereo mode: multichannel recordings are downmixed to 2 channels."
            : L"Explicit channel processing. Matrix sums may require preamp headroom.";
        {
            std::lock_guard<std::mutex> lock(metadata_mutex);
            current_info = decoder->Info(); current_path = path; current_output_note = std::move(note);
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
void __cdecl Pause() { paused.store(true); if (output_open) module.outMod->Pause(1); if (control_event) SetEvent(control_event); }
void __cdecl UnPause() { if (output_open) module.outMod->Pause(0); paused.store(false); if (control_event) SetEvent(control_event); }
int __cdecl IsPaused() { return paused.load() ? 1 : 0; }
int __cdecl GetLength() { return length_ms.load(); }
int __cdecl GetOutputTime() {
    if (!output_open) return 0;
    const auto time = static_cast<std::int64_t>(decoded_ms.load()) + module.outMod->GetOutputTime() - module.outMod->GetWrittenTime();
    const int length = length_ms.load();
    return static_cast<int>(std::clamp<std::int64_t>(time, 0, length >= 0 ? length : INT_MAX));
}
void __cdecl SetOutputTime(int time) {
    if (output_open && module.is_seekable && length_ms.load() >= 0) {
        seek_ms.store(std::clamp(time, 0, length_ms.load()));
        if (control_event) SetEvent(control_event);
    }
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
            int channels = (settings.output_mode == OutputMode::SourceChannels || settings.output_mode == OutputMode::LegacyUnmapped) && info.native_channels > 0 ? info.native_channels : 2;
            if(settings.output_mode==OutputMode::Mono)channels=1;
            else if(settings.output_mode==OutputMode::Quad)channels=4;
            std::wstring note = L"Planned format; output support is checked when playback starts.";
            {
                std::lock_guard<std::mutex> lock(metadata_mutex);
                if (active_channels > 0 && (!path || !*path || _wcsicmp(path, current_path.c_str()) == 0)) {
                    channels = active_channels; note = current_output_note; settings = active_settings;
                } else if (settings.output_mode == OutputMode::SourceChannels && info.native_channels == 0)
                    note = info.radio ? L"Live radio uses stereo. Start playback to read station information."
                        : L"Stereo fallback: the channel layout changes within the file or is unsupported.";
            }
            if (!settings.sample_rate) settings.sample_rate = info.output_rate;
            ShowFileInfoDialog(module.hDllInstance, parent, path, info, channels, note, settings.sample_rate, settings.bits);
        }
    } catch (...) { }
    return 1;
}
void __cdecl About(HWND parent) {
    MessageBoxW(parent, L"Winamp Opus Input 1.5.0 (32-bit)\n\n"
        L"libopus 1.6.1 / libopusfile 0.12 / libogg 1.3.6 / SpeexDSP 1.2.1\n"
        L"Local Ogg Opus and HTTP(S) radio, Unicode, ReplayGain/R128, seeking, DSP and visualization.\n"
        L"Native Opus HD/QEXT, optional OSCE/BWE, Ambisonics and bounded recovery.\n"
        L"Source/mono/stereo/quad/matrix PCM; safe OpusTags editing and integer/float extended read.\n"
        L"Optional ICY, recording, reconnect, HTTP seeking and host settings.\n"
        L"Audio libraries are static; the C/C++ runtime is loaded from DLLs.",
        L"About Winamp Opus Input", MB_OK | MB_ICONINFORMATION);
}
void __cdecl Config(HWND parent) {
    try {
        InitializeLanguage(module.hMainWindow,module.hDllInstance);
        auto settings = Settings(); std::wstring status = Localized(L"Playback is not currently running.");
        { std::lock_guard<std::mutex> lock(metadata_mutex);
          if (active_channels > 0) status = OutputFormatText(active_channels, active_settings.sample_rate, active_settings.bits); }
        settings = ShowPlaybackConfigDialog(module.hDllInstance, parent, settings, status);
        { std::lock_guard<std::mutex> lock(metadata_mutex); configured_settings = settings; metadata_cache = {}; }
    } catch (...) { }
}
void __cdecl Init() {
    InitializeLanguage(module.hMainWindow,module.hDllInstance);
    ConfigureTagEditing(module.hMainWindow,InvalidateMetadata);
    std::lock_guard<std::mutex> lock(metadata_mutex); configured_settings = LoadPlaybackSettings();
}
void __cdecl Quit() { Stop(); ConfigureTagEditing(nullptr,nullptr); ShutdownLanguage(); }
void __cdecl EQSet(int, char[10], int) { }
winamp::InModule MakeModule() noexcept {
    static char description[] = "Winamp Opus Input 1.5.0 (x86)";
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
