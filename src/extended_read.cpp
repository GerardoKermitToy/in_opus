// SPDX-License-Identifier: GPL-3.0-only
#include "opus_decoder.hpp"
#include "pcm_pipeline.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {
// No playback globals, persisted preferences, host ReplayGain, DSP or output
// plug-in callbacks participate in a decode-only session.
struct ExtendedRead {
    winamp_opus::Decoder decoder;
    std::unique_ptr<winamp_opus::PcmPipeline> pcm;
    std::mutex mutex;
    std::array<float, 576 * winamp_opus::kMaxChannels> block{};
    int frameBytes = 0;
    bool floating = false;
};
std::mutex sessionsMutex;
std::unordered_map<std::intptr_t, std::shared_ptr<ExtendedRead>> sessions;
std::uintptr_t nextHandle = 1;
std::shared_ptr<ExtendedRead> Session(std::intptr_t handle) {
    std::lock_guard<std::mutex> guard(sessionsMutex);
    const auto found = sessions.find(handle);
    return found == sessions.end() ? nullptr : found->second;
}
std::intptr_t Register(std::shared_ptr<ExtendedRead> reader) {
    std::lock_guard<std::mutex> guard(sessionsMutex);
    if (nextHandle > static_cast<std::uintptr_t>((std::numeric_limits<std::intptr_t>::max)())) return 0;
    const auto handle = static_cast<std::intptr_t>(nextHandle++);
    sessions.emplace(handle, std::move(reader));
    return handle;
}
bool LocalFile(const wchar_t* path) {
    if (!path || !*path || std::wcsstr(path, L"://") || !std::wcsncmp(path, L"\\\\.\\", 4) ||
        !std::wcsncmp(path, L"\\\\?\\GLOBALROOT", 14)) return false;
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::intptr_t Open(const wchar_t* path, int* size, int* bits, int* channels, int* rate, bool floating) {
    if (!path || !size || !bits || !channels || !rate || !LocalFile(path)) return 0;
    // Decoder::Open performs strict content validation, independent of suffix:
    // Opus in .ogg/.oga is accepted; Vorbis/FLAC/Speex/Matroska are not.
    winamp_opus::PlaybackSettings settings;
    settings.bits = floating ? 32 : (*bits == 8 || *bits == 16 || *bits == 24 || *bits == 32 ? *bits : 16);
    settings.sample_rate = *rate >= 8000 && *rate <= 192000 ? *rate : 0;
    settings.output_mode = *channels == 1 ? winamp_opus::OutputMode::Mono :
        *channels == 2 ? winamp_opus::OutputMode::Stereo :
        *channels == 4 ? winamp_opus::OutputMode::Quad : winamp_opus::OutputMode::SourceChannels;
    settings.dither = !floating && settings.bits != 32;
    auto reader = std::make_shared<ExtendedRead>();
    if (!reader->decoder.Open(path, nullptr, settings)) return 0;
    if (!reader->decoder.SetOutputMode(settings.output_mode))
        reader->decoder.SetOutputMode(winamp_opus::OutputMode::Stereo); // Mixed-link layouts use a stable stereo source.
    reader->pcm = std::make_unique<winamp_opus::PcmPipeline>(reader->decoder, settings);
    if (!reader->pcm->Ready()) return 0;
    const int outputChannels = reader->pcm->Channels(), outputRate = reader->pcm->Rate();
    if (outputChannels <= 0 || outputChannels > winamp_opus::kMaxChannels || outputRate <= 0) return 0;
    reader->floating = floating;
    reader->frameBytes = outputChannels * (settings.bits / 8);
    const auto frames = reader->decoder.Info().total_frames; // 48 kHz granule frames, also for QEXT.
    int outputSize = -1;
    if (frames >= 0) {
        const auto seconds = frames / winamp_opus::kSampleRate;
        const auto remainder = frames % winamp_opus::kSampleRate;
        const auto limit = static_cast<std::int64_t>(INT_MAX) / reader->frameBytes;
        if (seconds <= limit / outputRate) {
            const auto outputFrames = seconds * outputRate +
                remainder * outputRate / winamp_opus::kSampleRate;
            if (outputFrames <= limit) outputSize = static_cast<int>(outputFrames * reader->frameBytes);
        }
    }
    const auto handle = Register(std::move(reader));
    if (!handle) return 0;
    *size = outputSize; *bits = settings.bits; *channels = outputChannels; *rate = outputRate;
    return handle;
}
bool Killed(const int* flag) noexcept {
    // Winamp owns this cooperative cancellation flag. Reload it each block;
    // never clear it or retain the pointer beyond this getData call.
    return flag && *static_cast<const volatile int*>(flag) != 0;
}
} // namespace

extern "C" std::intptr_t __cdecl winampGetExtendedRead_openW(
    const wchar_t* path, int* size, int* bits, int* channels, int* rate) {
    try { return Open(path, size, bits, channels, rate, false); } catch (...) { return 0; }
}
extern "C" std::intptr_t __cdecl winampGetExtendedRead_openW_float(
    const wchar_t* path, int* size, int* bits, int* channels, int* rate) {
    try { return Open(path, size, bits, channels, rate, true); } catch (...) { return 0; }
}
extern "C" std::intptr_t __cdecl winampGetExtendedRead_open(
    const char* path, int* size, int* bits, int* channels, int* rate) {
    if (!path) return 0;
    try {
        const int sizeNeeded = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        if (sizeNeeded <= 0 || sizeNeeded > 32768) return 0;
        std::wstring wide(sizeNeeded, 0);
        if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), sizeNeeded) != sizeNeeded) return 0;
        return Open(wide.c_str(), size, bits, channels, rate, false);
    } catch (...) { return 0; }
}
extern "C" std::size_t __cdecl winampGetExtendedRead_getData(
    std::intptr_t handle, char* buffer, std::size_t capacity, int* kill) {
    if (!buffer || Killed(kill)) return 0;
    std::size_t used = 0;
    try {
        auto reader = Session(handle);
        if (!reader) return 0;
        std::lock_guard<std::mutex> guard(reader->mutex);
        const auto frameBytes = static_cast<std::size_t>(reader->frameBytes);
        while (capacity - used >= frameBytes && !Killed(kill)) {
            const int frames = static_cast<int>((std::min)(std::size_t(576), (capacity - used) / frameBytes));
            // A Winamp caller may supply an unaligned byte buffer. Decode into
            // our aligned block and copy only complete interleaved PCM frames.
            const int count = reader->floating ? reader->pcm->ReadFloat(reader->block.data(), frames) :
                reader->pcm->Read(reader->block.data(), frames);
            if (count <= 0 || count > frames) break;
            const auto bytes = static_cast<std::size_t>(count) * frameBytes;
            std::memcpy(buffer + used, reader->block.data(), bytes);
            used += bytes;
        }
    } catch (...) { /* Preserve the bytes successfully delivered before failure. */ }
    return used;
}
extern "C" int __cdecl winampGetExtendedRead_setTime(std::intptr_t handle, int milliseconds) {
    if (milliseconds < 0) return 0;
    try {
        auto reader = Session(handle);
        if (!reader) return 0;
        std::lock_guard<std::mutex> guard(reader->mutex);
        if (!reader->decoder.SeekMs(milliseconds)) return 0;
        reader->pcm->ResetAfterSeek();
        return 1;
    } catch (...) { return 0; }
}
extern "C" void __cdecl winampGetExtendedRead_close(std::intptr_t handle) {
    try {
        // Existing getData calls hold shared ownership; close can safely race
        // them, and stale/duplicate handles can never refer to another session.
        std::shared_ptr<ExtendedRead> reader;
        {
            std::lock_guard<std::mutex> guard(sessionsMutex);
            const auto found = sessions.find(handle);
            if (found == sessions.end()) return;
            reader = std::move(found->second); sessions.erase(found);
        }
    } catch (...) {}
}
