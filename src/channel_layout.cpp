#include "channel_layout.hpp"
#include <algorithm>
#include <array>

namespace winamp_opus {
void ReorderToWindows(std::int16_t* pcm, int frames, int channels) noexcept {
    if (channels <= 2 || channels > kMaxChannels) return;
    // Each Windows output channel selects this index in an Opus/Vorbis frame.
    static constexpr int order[8][8]{
        {0}, {0,1}, {0,2,1}, {0,1,2,3}, {0,2,1,3,4},
        {0,2,1,5,3,4}, {0,2,1,6,5,3,4}, {0,2,1,7,5,6,3,4}};
    std::array<std::int16_t, kMaxChannels> frame{};
    for (int i = 0; i < frames; ++i) {
        auto* samples = pcm + i * channels;
        std::copy_n(samples, channels, frame.begin());
        for (int ch = 0; ch < channels; ++ch) samples[ch] = frame[order[channels - 1][ch]];
    }
}
} // namespace winamp_opus
