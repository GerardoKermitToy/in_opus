#pragma once
#include "opus_decoder.hpp"
#include <cstddef>
#include <memory>

namespace winamp_opus {
// A bounded native Ogg reader uses the plugin's existing local/HTTP transport.
// Position and seeks below always use the Ogg Opus 48 kHz timeline, not output Hz.
class NativeInput {
public:
    virtual ~NativeInput() = default;
    virtual int Read(unsigned char*, int bytes) noexcept = 0;
    virtual bool Seek(std::int64_t, int origin) noexcept = 0;
    virtual std::int64_t Tell() const noexcept = 0;
    virtual bool Seekable() const noexcept = 0;
    virtual void Cancel() noexcept = 0;
    // A seekable transport may finish its bounded network identification once
    // the first validated Opus audio batch is parsed, before indexing the rest.
    virtual void ProbeComplete() noexcept {}
};
class NativeOpus {
public:
    virtual ~NativeOpus() = default;
    virtual int Read(float*, int capacity_frames) = 0;
    virtual bool SeekFrame(std::int64_t frames48) = 0;
    virtual std::int64_t PositionFrames() const noexcept = 0;
    virtual int SampleRate() const noexcept = 0;
    virtual int OutputChannels() const noexcept = 0;
    virtual bool SetOutputMode(OutputMode) = 0;
    virtual void SetGain(const PlaybackSettings&, bool shuffle) = 0;
    virtual const TrackInfo& Info() const noexcept = 0;
    virtual unsigned MetadataRevision() const noexcept = 0;
    virtual int InstantBitrate() noexcept = 0;
    // False for an ordinary, unmodified local stream eligible for the legacy
    // libopusfile integer/dither path. All live inputs stay on this transport.
    virtual bool Needed() const noexcept = 0;
    virtual const std::wstring& Error() const noexcept = 0;
};
std::unique_ptr<NativeOpus> OpenNativeOpus(std::shared_ptr<NativeInput>,
    const PlaybackSettings&, std::wstring& error);
bool IsNativeOpusHead(const unsigned char*, std::size_t bytes) noexcept;
} // namespace winamp_opus
