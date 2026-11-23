// SPDX-License-Identifier: GPL-3.0-only
#include "processing.hpp"
#ifndef WINAMP_OPUS_PROCESSING_UNIT_ONLY
#include "fixtures.hpp"
#include "pcm_pipeline.hpp"
#include "gain_policy.hpp"
#include <ogg/ogg.h>
#include <cstring>
#include <fstream>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace winamp_opus;
namespace {
constexpr double k = 0.7071067811865475244;
constexpr std::array<int, 4> depths{8, 16, 24, 32};
constexpr std::array<DitherPdf, 3> pdfs{
    DitherPdf::Rectangular, DitherPdf::Triangular, DitherPdf::Gaussian};
constexpr std::array<NoiseShape, 4> shapes{
    NoiseShape::None, NoiseShape::FirstOrder, NoiseShape::Ath, NoiseShape::AthLow};
constexpr std::array<OutputMode, 7> modes{
    OutputMode::SourceChannels, OutputMode::Stereo, OutputMode::Mono, OutputMode::Quad,
    OutputMode::LegacyUnmapped, OutputMode::Surround, OutputMode::SurroundII};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Close(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 2e-6 * (1 + std::abs(expected)))
        throw std::runtime_error(message);
}
std::int64_t Decode(const unsigned char* pcm, int bits) {
    if (bits == 8) return static_cast<int>(pcm[0]) - 128;
    std::uint64_t value = 0;
    for (int b = 0; b < bits / 8; ++b) value |= static_cast<std::uint64_t>(pcm[b]) << (8 * b);
    const auto signed_value = static_cast<std::int64_t>(value);
    return (value & (UINT64_C(1) << (bits - 1))) ?
           signed_value - (INT64_C(1) << bits) : signed_value;
}
std::vector<unsigned char> Packed(Quantizer& quantizer, const std::vector<float>& input,
                                  int channels, int bits) {
    std::vector<unsigned char> result(input.size() * static_cast<std::size_t>(bits / 8));
    quantizer.Pack(result.data(), input.data(), static_cast<int>(input.size() / channels));
    return result;
}

void PackingAndBounds() {
    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    const float inf = (std::numeric_limits<float>::infinity)();
    const float extreme = (std::numeric_limits<float>::max)();
    const std::vector<float> input{-2, -1, -.5f, 0, .5f, 1, 2, nan, inf, -inf, extreme, -extreme};
    for (int bits : depths) {
        PlaybackSettings settings;
        settings.bits = bits;
        settings.dither = false;
        Quantizer quantizer(settings, 1, 48000);
        Require(quantizer.EffectiveShape() == NoiseShape::None, "disabled dither disables shaping");
        const auto scale = INT64_C(1) << (bits - 1);
        const std::array<std::int64_t, 12> expected{
            -scale, -scale, -scale / 2, 0, scale / 2, scale - 1, scale - 1,
            0, 0, 0, scale - 1, -scale};
        constexpr std::size_t guard = 19;
        const std::size_t size = input.size() * static_cast<std::size_t>(bits / 8);
        std::vector<unsigned char> buffer(guard + size + guard, 0xa5);
        quantizer.Pack(buffer.data() + guard, input.data(), static_cast<int>(input.size()));
        Require(std::all_of(buffer.begin(), buffer.begin() + guard, [](auto x) { return x == 0xa5; }) &&
              std::all_of(buffer.end() - guard, buffer.end(), [](auto x) { return x == 0xa5; }),
              "Pack writes exactly frames*channels*bytes");
        for (std::size_t i = 0; i < input.size(); ++i)
            Require(Decode(buffer.data() + guard + i * (bits / 8), bits) == expected[i],
                  "all PCM depths, signedness, finite handling and saturation");
        if (bits == 8) {
            Require(buffer[guard] == 0 && buffer[guard + 3] == 128 && buffer[guard + 5] == 255,
                  "8-bit PCM uses unsigned offset binary");
        } else {
            for (int b = 0; b < bits / 8; ++b)
                Require(buffer[guard + 5 * (bits / 8) + b] == (b == bits / 8 - 1 ? 0x7f : 0xff),
                      "signed PCM positive rail is packed little-endian");
        }
        const std::vector<float> ties{
            static_cast<float>(.5 / scale), static_cast<float>(-.5 / scale),
            static_cast<float>(1.5 / scale), static_cast<float>(-1.5 / scale)};
        const auto tied = Packed(quantizer, ties, 1, bits);
        const std::array<std::int64_t, 4> rounded{1, 0, 2, -1};
        for (int i = 0; i < 4; ++i)
            Require(Decode(tied.data() + i * (bits / 8), bits) == rounded[i], "defined half-LSB rounding");
    }
    PlaybackSettings invalid;
    invalid.dither = false;
    invalid.bits = 7;
    Quantizer fallback(invalid, 1, 48000);
    const std::vector<float> zero{0};
    Require(Packed(fallback, zero, 1, 16) == std::vector<unsigned char>({0, 0}),
          "invalid bit depth falls back to PCM16");
    unsigned char sentinel = 0x5a;
    for (int channels : {0, -1, 9}) {
        Quantizer no_channels(invalid, channels, 48000);
        no_channels.Pack(&sentinel, zero.data(), 1);
    }
    fallback.Pack(nullptr, zero.data(), 1);
    fallback.Pack(&sentinel, nullptr, 1);
    fallback.Pack(&sentinel, zero.data(), 0);
    fallback.Pack(&sentinel, zero.data(), -1);
    Require(sentinel == 0x5a, "invalid buffers/counts are harmless no-ops");
}

void DeterminismAndChunking() {
    constexpr int channels = 8, frames = 769;
    std::vector<float> input(frames * channels);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c)
            input[f * channels + c] = static_cast<float>(.6 * std::sin(.031 * f + .29 * c));
    for (int bits : depths) for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        PlaybackSettings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer whole(settings, channels, 48000), chunked(settings, channels, 48000);
        const auto expected = Packed(whole, input, channels, bits);
        std::vector<unsigned char> actual(expected.size(), 0);
        int offset = 0, iteration = 0;
        constexpr std::array<int, 7> chunks{1, 2, 31, 7, 128, 3, 257};
        while (offset < frames) {
            const int count = (std::min)(chunks[iteration++ % chunks.size()], frames - offset);
            chunked.Pack(actual.data() + offset * channels * (bits / 8),
                         input.data() + offset * channels, count);
            offset += count;
        }
        Require(expected == actual, "all PDFs/shapes/depths are chunk-size independent");
        whole.Reset();
        Require(Packed(whole, input, channels, bits) == expected, "Reset restores RNG and channel histories");
        Quantizer independent(settings, channels, 48000);
        Require(Packed(independent, input, channels, bits) == expected, "instances have deterministic private seeds");
        independent.Reset();
        independent.Pack(nullptr, input.data(), frames);
        independent.Pack(actual.data(), input.data(), 0);
        Require(Packed(independent, input, channels, bits) == expected, "no-op calls do not advance state");
    }
}

void ChannelIndependence() {
    constexpr int frames = 16384, channels = 8;
    std::vector<float> silence(frames * channels, 0), changed(silence), mono(frames, 0);
    for (int f = 0; f < frames; ++f) {
        changed[f * channels + 3] = f & 1 ? 20.f : -20.f;
        changed[f * channels + 6] = .4f;
    }
    for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        PlaybackSettings settings;
        settings.bits = 16; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer a(settings, channels, 48000), b(settings, channels, 48000), one(settings, 1, 48000);
        const auto original = Packed(a, silence, channels, 16);
        const auto altered = Packed(b, changed, channels, 16);
        const auto first = Packed(one, mono, 1, 16);
        for (int f = 0; f < frames; ++f) {
            Require(Decode(original.data() + f * channels * 2, 16) == Decode(first.data() + f * 2, 16),
                  "channel zero's RNG is independent of configured channel count");
            for (int c : {0, 1, 2, 4, 5, 7})
                Require(Decode(original.data() + (f * channels + c) * 2, 16) ==
                      Decode(altered.data() + (f * channels + c) * 2, 16),
                      "other channels and their overloads cannot alter a channel's state");
        }
        // Rectangular dither on exact silence rounds to zero, so use the two
        // nondegenerate zero-input distributions to check stream separation.
        if (pdf != DitherPdf::Rectangular) {
            int differences = 0;
            double cross = 0., energy0 = 0., energy1 = 0.;
            for (int f = 0; f < frames; ++f) {
                const double x = static_cast<double>(Decode(original.data() + f * channels * 2, 16));
                const double y = static_cast<double>(Decode(original.data() + (f * channels + 1) * 2, 16));
                differences += x != y ? 1 : 0;
                cross += x * y; energy0 += x * x; energy1 += y * y;
            }
            Require(differences > frames / 10, "channel seeds generate distinct noise streams");
            Require(std::abs(cross) < .08 * std::sqrt(energy0 * energy1), "channel noise is not correlated");
        }
    }
}

void DistributionStatistics() {
    constexpr int count = 131072, phases = 4096;
    for (int bits : depths) for (DitherPdf pdf : pdfs) {
        PlaybackSettings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = NoiseShape::None;
        Quantizer quantizer(settings, 1, 48000);
        const double scale = std::ldexp(1., bits - 1);
        std::vector<float> input(count);
        // Dyadic quantization phases are represented exactly at every tested
        // bit depth, including PCM32. Uniform phases expose dither variance:
        // Var(total error) = Var(dither) + 1/12 for unshaped rounding.
        for (int i = 0; i < count; ++i) {
            const double phase = ((i * 2053 % phases) + .5) / phases - .5;
            input[i] = static_cast<float>(phase / scale);
        }
        const auto output = Packed(quantizer, input, 1, bits);
        double sum = 0., square = 0.;
        for (int i = 0; i < count; ++i) {
            const double error = static_cast<double>(Decode(output.data() + i * (bits / 8), bits)) - input[i] * scale;
            sum += error; square += error * error;
        }
        const double mean = sum / count;
        const double variance = square / count - mean * mean;
        const double dither_variance = pdf == DitherPdf::Rectangular ? 1. / 12 : 1. / 6;
        Require(std::abs(mean) < .008, "each distribution is statistically zero-mean at all depths");
        Require(std::abs(variance - (dither_variance + 1. / 12)) < .008,
              "rectangular/triangular/Gaussian statistical variances match their definitions");
    }
    for (DitherPdf pdf : pdfs) {
        PlaybackSettings settings;
        settings.bits = 16; settings.dither_pdf = pdf; settings.noise_shape = NoiseShape::None;
        Quantizer quantizer(settings, 1, 48000);
        const std::vector<float> silence(count, 0);
        const auto output = Packed(quantizer, silence, 1, 16);
        int nonzero = 0, tails = 0;
        for (int i = 0; i < count; ++i) {
            const auto value = Decode(output.data() + i * 2, 16);
            nonzero += value != 0 ? 1 : 0;
            tails += std::abs(value) > 1 ? 1 : 0;
        }
        if (pdf == DitherPdf::Rectangular) Require(nonzero == 0, "rectangular half-LSB support");
        if (pdf == DitherPdf::Triangular) {
            Require(tails == 0, "triangular dither is bounded to one LSB");
            Require(std::abs(nonzero / static_cast<double>(count) - .25) < .008,
                  "triangular zero-input quantized variance");
        }
        if (pdf == DitherPdf::Gaussian) Require(tails > 3, "Gaussian has independently generated wider tails");
    }
}

void RatePoliciesAndOverload() {
    for (int rate : {-1, 0, 8000, 16000, 22050, 24000, 32000, 37800, 44100, 48000, 96000, 192000}) {
        for (NoiseShape shape : shapes) {
            PlaybackSettings settings;
            settings.noise_shape = shape;
            const bool ath = rate == 22050 || rate == 32000 || rate == 37800 || rate == 44100 || rate == 48000;
            const bool low = rate == 44100 || rate == 48000;
            NoiseShape expected = shape;
            if ((shape == NoiseShape::Ath && !ath) || (shape == NoiseShape::AthLow && !low))
                expected = NoiseShape::FirstOrder;
            Quantizer quantizer(settings, 1, rate);
            Require(quantizer.EffectiveShape() == expected, "ATH rates and explicit first-order fallbacks");
            if (expected == NoiseShape::FirstOrder && shape != NoiseShape::FirstOrder) {
                PlaybackSettings reference = settings;
                reference.noise_shape = NoiseShape::FirstOrder;
                Quantizer first_order(reference, 1, rate);
                std::vector<float> probe(257);
                for (int i = 0; i < static_cast<int>(probe.size()); ++i)
                    probe[i] = static_cast<float>(.05 * std::sin(.071 * i));
                Require(Packed(quantizer, probe, 1, 16) == Packed(first_order, probe, 1, 16),
                      "rate fallback uses exactly the first-order processor");
            }
            settings.dither = false;
            Require(Quantizer(settings, 1, rate).EffectiveShape() == NoiseShape::None,
                  "dither switch disables every shaping mode at every rate");
        }
    }
    for (int bits : depths) for (DitherPdf pdf : pdfs) for (NoiseShape shape : shapes) {
        PlaybackSettings settings;
        settings.bits = bits; settings.dither_pdf = pdf; settings.noise_shape = shape;
        Quantizer a(settings, 1, 48000), b(settings, 1, 48000);
        std::vector<float> positive(97, .125f), negative(97, -.125f);
        Packed(a, positive, 1, bits); Packed(b, negative, 1, bits);
        const std::vector<float> clipped{(std::numeric_limits<float>::max)()};
        Require(Packed(a, clipped, 1, bits) == Packed(b, clipped, 1, bits), "extreme input saturates consistently");
        const std::vector<float> silence(257, 0);
        Require(Packed(a, silence, 1, bits) == Packed(b, silence, 1, bits), "overload clears all shaping history");
        std::vector<float> extremes(4096);
        for (int i = 0; i < static_cast<int>(extremes.size()); ++i) {
            switch (i % 6) {
            case 0: extremes[i] = (std::numeric_limits<float>::max)(); break;
            case 1: extremes[i] = -(std::numeric_limits<float>::max)(); break;
            case 2: extremes[i] = (std::numeric_limits<float>::quiet_NaN)(); break;
            case 3: extremes[i] = (std::numeric_limits<float>::infinity)(); break;
            case 4: extremes[i] = (std::numeric_limits<float>::denorm_min)(); break;
            default: extremes[i] = 0; break;
            }
        }
        const auto output = Packed(a, extremes, 1, bits);
        const auto scale = INT64_C(1) << (bits - 1);
        for (int i = 0; i < static_cast<int>(extremes.size()); ++i) {
            const auto code = Decode(output.data() + i * (bits / 8), bits);
            if (i % 6 == 0) Require(code == scale - 1, "positive extreme has no integer overflow");
            else if (i % 6 == 1) Require(code == -scale, "negative extreme has no integer overflow");
            else Require(std::abs(code) < 32, "finite history recovers after nonfinite/extreme input");
        }
    }
}

using Matrix = std::array<std::array<double, 8>, 8>;
Matrix ExpectedMatrix(OutputMode mode, int source) {
    Matrix m{};
    if (mode == OutputMode::SourceChannels || mode == OutputMode::LegacyUnmapped ||
        (mode == OutputMode::Quad && source <= 4)) {
        for (int c = 0; c < source; ++c) m[c][c] = 1;
    } else if (mode == OutputMode::Mono) {
        for (int c = 0; c < source; ++c) m[0][c] = 1. / source;
    } else if (mode == OutputMode::Stereo) {
        m[0][0] = 1; m[1][source == 1 ? 0 : 1] = 1;
        if (source >= 3 && source != 4) m[0][2] = m[1][2] = k;
        switch (source) {
        case 4: m[0][2] = m[1][3] = k; break;
        case 5: m[0][3] = m[1][4] = k; break;
        case 6: m[0][4] = m[1][5] = k; break;
        case 7: m[0][4] = m[1][4] = .5; m[0][5] = m[1][6] = k; break;
        case 8: m[0][4] = m[0][6] = m[1][5] = m[1][7] = k; break;
        default: break;
        }
        const std::array<double, 8> divisor{1, 1, 1 + k, 1 + k, 1 + 2 * k, 1 + 2 * k,
                                            1.5 + 2 * k, 1 + 3 * k};
        for (int row = 0; row < 2; ++row) for (int c = 0; c < source; ++c) m[row][c] /= divisor[source - 1];
    } else if (mode == OutputMode::Quad) {
        m[0][0] = m[1][1] = 1;
        m[0][2] = m[1][2] = k;
        if (source >= 6) m[0][3] = m[1][3] = k;
        switch (source) {
        case 5: m[2][3] = m[3][4] = 1; break;
        case 6: m[2][4] = m[3][5] = 1; break;
        case 7: m[2][4] = m[3][4] = .5; m[2][5] = m[3][6] = 1; break;
        case 8: m[2][4] = m[2][6] = m[3][5] = m[3][7] = .5; break;
        default: break;
        }
    } else {
        m[0][0] = 1; m[1][source == 1 ? 0 : 1] = 1;
        if (source >= 3 && source != 4) m[0][2] = m[1][2] = k;
        if (source >= 6) m[0][3] = m[1][3] = k;
        // Speaker-to-rear-bus incidence, independent of the encoder equations.
        constexpr std::array<std::array<double, 8>, 8> left{{
            {}, {}, {}, {{0,0,1,0,0,0,0,0}}, {{0,0,0,1,0,0,0,0}},
            {{0,0,0,0,1,0,0,0}}, {{0,0,0,0,.5,1,0,0}}, {{0,0,0,0,1,0,1,0}}}};
        constexpr std::array<std::array<double, 8>, 8> right{{
            {}, {}, {}, {{0,0,0,1,0,0,0,0}}, {{0,0,0,0,1,0,0,0}},
            {{0,0,0,0,0,1,0,0}}, {{0,0,0,0,.5,0,1,0}}, {{0,0,0,0,0,1,0,1}}}};
        const double near_weight = mode == OutputMode::SurroundII ? std::pow(10., 1.5 / 20) : 1.;
        const double far_weight = mode == OutputMode::SurroundII ? std::pow(10., -1.5 / 20) : 1.;
        for (int c = 0; c < source; ++c) {
            m[0][c] -= k * (near_weight * left[source - 1][c] + far_weight * right[source - 1][c]);
            m[1][c] += k * (far_weight * left[source - 1][c] + near_weight * right[source - 1][c]);
        }
    }
    return m;
}

void ChannelMatrices() {
    for (OutputMode mode : modes) for (int source = 1; source <= 8; ++source) {
        const int expected_channels = mode == OutputMode::Mono ? 1 :
            mode == OutputMode::Stereo || mode == OutputMode::Surround || mode == OutputMode::SurroundII ? 2 :
            mode == OutputMode::Quad && source >= 4 ? 4 : source;
        Require(OutputChannels(mode, source) == expected_channels, "every mode's 1..8 output channel counts");
        const Matrix matrix = ExpectedMatrix(mode, source);
        for (int c = 0; c < source; ++c) {
            std::array<float, 8> impulse{};
            impulse[c] = 1;
            std::array<float, 10> guarded{};
            guarded.front() = 123; guarded[expected_channels + 1] = 456;
            MixFrame(impulse.data(), source, guarded.data() + 1, expected_channels, mode, 1.7);
            Require(guarded.front() == 123 && guarded[expected_channels + 1] == 456, "matrix output guards");
            for (int row = 0; row < expected_channels; ++row)
                Close(guarded[row + 1], 1.7 * matrix[row][c], "every impulse's matrix sign, coefficient and gain");
            auto in_place = impulse;
            MixFrame(in_place.data(), source, in_place.data(), expected_channels, mode, 1.7);
            for (int row = 0; row < expected_channels; ++row)
                Close(in_place[row], guarded[row + 1], "in-place matrix mixing");
        }
        std::array<float, 8> ones{}, out{};
        ones.fill(1);
        MixFrame(ones.data(), source, out.data(), expected_channels, mode, 1.);
        if (mode == OutputMode::Stereo || mode == OutputMode::Mono)
            for (int row = 0; row < expected_channels; ++row) Close(out[row], 1, "plain stereo/mono are normalized");
    }
    for (OutputMode mode : modes) {
        Require(OutputChannels(mode, 0) == 0 && OutputChannels(mode, 9) == 0, "invalid layout counts are rejected");
    }
    Require(OutputChannels(static_cast<OutputMode>(99), 6) == 6, "unknown mode safely preserves count");
    std::array<float, 8> raw{1, 2, 3, 4, 5, 6, 7, 8}, out{};
    MixFrame(raw.data(), 6, out.data(), 6, OutputMode::LegacyUnmapped, 1.);
    Require(std::equal(raw.begin(), raw.begin() + 6, out.begin()), "raw six-channel legacy order is not reordered");
    MixFrame(raw.data(), 3, out.data(), 8, OutputMode::SourceChannels, 1.);
    Require(out[0] == 1 && out[2] == 3 && out[3] == 0 && out[7] == 0, "explicit count mismatch zero-pads pass-through");
    MixFrame(raw.data(), 8, out.data(), 2, OutputMode::SourceChannels, 1.);
    Require(out[0] == 1 && out[1] == 2, "explicit count mismatch truncates pass-through");
    raw.fill(0); raw[4] = 1;
    std::array<float, 2> rear_left{}, rear_right{};
    MixFrame(raw.data(), 6, rear_left.data(), 2, OutputMode::SurroundII, 1.);
    raw[4] = 0; raw[5] = 1;
    MixFrame(raw.data(), 6, rear_right.data(), 2, OutputMode::SurroundII, 1.);
    Require(rear_left[0] < 0 && rear_left[1] > 0 && rear_right[0] < 0 && rear_right[1] > 0,
          "DS2 rear signals are anti-phase");
    Require(std::abs(rear_left[0]) > std::abs(rear_right[0]) && rear_right[1] > rear_left[1],
          "DS2 preserves asymmetric rear separation");
    Close(-rear_left[0] / rear_left[1], std::pow(10., 3. / 20.), "DS2 rear ratio is independently derived +/-1.5dB");
    raw.fill(1);
    MixFrame(raw.data(), 6, out.data(), 2, OutputMode::Surround, 1.);
    Require(out[1] > 3.8f, "DS is unnormalized before packing");
    PlaybackSettings settings; settings.dither = false; settings.bits = 16;
    Quantizer quantizer(settings, 2, 48000);
    std::array<unsigned char, 4> packed{};
    quantizer.Pack(packed.data(), out.data(), 1);
    Require(Decode(packed.data() + 2, 16) == 32767, "DS over-range matrix clips at integer pack only");
    raw[0] = (std::numeric_limits<float>::quiet_NaN)();
    raw[1] = (std::numeric_limits<float>::infinity)();
    raw[2] = (std::numeric_limits<float>::max)();
    for (OutputMode mode : modes) {
        const int count = OutputChannels(mode, 8);
        MixFrame(raw.data(), 8, out.data(), count, mode, (std::numeric_limits<double>::max)());
        for (int c = 0; c < count; ++c) Require(std::isfinite(out[c]), "extreme mix/gain stays within finite float range");
        MixFrame(raw.data(), 8, out.data(), count, mode, (std::numeric_limits<double>::quiet_NaN)());
        for (int c = 0; c < count; ++c) Require(out[c] == 0, "nonfinite gain becomes silence");
    }
    MixFrame(nullptr, 6, out.data(), 2, OutputMode::Stereo, 1.);
    Require(out[0] == 0 && out[1] == 0, "missing mix input becomes silence");
}

double BandPower(const std::vector<double>& noise, double first, double last) {
    constexpr int probes = 48;
    double total = 0;
    for (int probe = 0; probe < probes; ++probe) {
        const double frequency = first + (last - first) * (probe + .5) / probes;
        const double bin = std::floor(frequency * static_cast<double>(noise.size()) / 48000 + .5);
        const double coefficient = 2 * std::cos(2 * 3.14159265358979323846 * bin / static_cast<double>(noise.size()));
        double previous = 0., previous2 = 0.;
        for (double value : noise) {
            const double current = value + coefficient * previous - previous2;
            previous2 = previous; previous = current;
        }
        total += previous * previous + previous2 * previous2 - coefficient * previous * previous2;
    }
    return total / probes;
}
void ShapingSpectrum() {
    constexpr int warmup = 512, count = 32768;
    double ath_energy = 0, low_energy = 0;
    for (NoiseShape shape : {NoiseShape::FirstOrder, NoiseShape::Ath, NoiseShape::AthLow}) {
        PlaybackSettings settings;
        settings.bits = 16; settings.noise_shape = shape; settings.dither_pdf = DitherPdf::Triangular;
        Quantizer quantizer(settings, 1, 48000);
        const std::vector<float> silence(warmup + count, 0);
        const auto output = Packed(quantizer, silence, 1, 16);
        std::vector<double> noise(count);
        double energy = 0;
        for (int i = 0; i < count; ++i) {
            noise[i] = static_cast<double>(Decode(output.data() + (warmup + i) * 2, 16));
            energy += noise[i] * noise[i];
        }
        Require(BandPower(noise, 1000, 4000) < .8 * BandPower(noise, 13000, 19000),
              "first-order and analytic ATH filters move noise out of sensitive frequencies");
        if (shape == NoiseShape::Ath) ath_energy = energy;
        if (shape == NoiseShape::AthLow) low_energy = energy;
    }
    Require(low_energy < ath_energy, "low-amplitude ATH has lower total output noise than full ATH");
}
#ifndef WINAMP_OPUS_PROCESSING_UNIT_ONLY
constexpr int kWindowsFromCodec[8][8]{
    {0}, {0,1}, {0,2,1}, {0,1,2,3}, {0,2,1,3,4},
    {0,2,1,5,3,4}, {0,2,1,6,5,3,4}, {0,2,1,7,5,6,3,4}};
void OpenFixture(Decoder& decoder, const std::filesystem::path& path, PlaybackSettings settings) {
    // Exercise the actual decoder+PCM contract, not a fake Decoder substitute.
    // Custom matrices need source speakers; Stereo remains decoder-owned.
    settings.output_mode = settings.output_mode == OutputMode::Stereo ? OutputMode::Stereo : OutputMode::SourceChannels;
    Require(decoder.Open(path.c_str(), nullptr, settings), "open processing fixture");
    Require(decoder.SetOutputMode(settings.output_mode), "select stable fixture source speakers");
}
std::vector<float> DecoderFloat(Decoder& decoder) {
    std::array<float, 576 * 8> block{};
    std::vector<float> result;
    for (;;) {
        const int n = decoder.ReadFloatFrames(block.data(), 576);
        Require(n >= 0 && n <= 576, "reference float decoder read");
        if (!n) return result;
        result.insert(result.end(), block.begin(), block.begin() + n * decoder.OutputChannels());
    }
}
std::vector<float> PipelineFloat(PcmPipeline& pipeline) {
    std::array<float, 576 * 8 + 2> block{};
    std::vector<float> result;
    int iteration = 0;
    constexpr std::array<int, 5> chunks{1, 113, 576, 7, 251};
    auto position = pipeline.PositionFrames();
    for (;;) {
        const int capacity = chunks[iteration++ % chunks.size()];
        block.front() = 12345;
        block[1 + capacity * pipeline.Channels()] = -12345;
        const int n = pipeline.ReadFloat(block.data() + 1, capacity);
        Require(n >= 0 && n <= capacity, "bounded float pipeline read");
        Require(block.front() == 12345 && block[1 + capacity * pipeline.Channels()] == -12345,
                "float output writes only final-channel capacity");
        Require(pipeline.PositionFrames() >= position, "float pipeline position is monotonic");
        position = pipeline.PositionFrames();
        if (!n) return result;
        result.insert(result.end(), block.begin() + 1, block.begin() + 1 + n * pipeline.Channels());
    }
}
std::vector<unsigned char> PipelinePcm(PcmPipeline& pipeline, int bits, int capacity = 576) {
    std::array<std::uint32_t, 576 * 8 + 2> storage{};
    auto* pcm = reinterpret_cast<unsigned char*>(storage.data() + 1);
    const int bytes = capacity * pipeline.Channels() * (bits / 8);
    std::vector<unsigned char> result;
    for (;;) {
        storage[0] = 0x12345678;
        std::memset(pcm + bytes, 0x5a, 4);
        const int n = pipeline.Read(pcm, capacity);
        Require(n >= 0 && n <= capacity, "bounded integer pipeline read");
        Require(storage[0] == 0x12345678, "integer pipeline lower guard");
        for (int b = 0; b < 4; ++b) Require(pcm[bytes + b] == 0x5a, "integer pipeline upper guard");
        if (!n) return result;
        result.insert(result.end(), pcm, pcm + n * pipeline.Channels() * (bits / 8));
    }
}
std::vector<float> ExpectedMixed(const std::vector<float>& source, int channels, OutputMode mode) {
    const int output = OutputChannels(mode, channels);
    std::vector<float> result(source.size() / channels * output);
    const auto matrix = ExpectedMatrix(mode, channels);
    for (std::size_t f = 0; f < source.size() / channels; ++f) {
        if (mode == OutputMode::LegacyUnmapped) {
            for (int c = 0; c < channels; ++c)
                result[f * output + kWindowsFromCodec[channels - 1][c]] = source[f * channels + c];
        } else {
            for (int row = 0; row < output; ++row) {
                double value = 0;
                for (int c = 0; c < channels; ++c) value += matrix[row][c] * source[f * channels + c];
                result[f * output + row] = static_cast<float>(value);
            }
        }
    }
    return result;
}
void RealMatricesAndQuantizer(const std::array<std::filesystem::path, 8>& paths) {
    for (const auto& path : paths) for (const auto mode : modes) {
        PlaybackSettings s;
        s.output_mode = mode;
        s.custom_quantizer = true;
        s.dither_pdf = DitherPdf::Gaussian;
        s.noise_shape = NoiseShape::AthLow;
        Decoder reference, decoded;
        OpenFixture(reference, path, s);
        OpenFixture(decoded, path, s);
        const int source = reference.OutputChannels();
        const auto expected = ExpectedMixed(DecoderFloat(reference), source, mode);
        PcmPipeline pipeline(decoded, s);
        Require(pipeline.Ready() && pipeline.Channels() == OutputChannels(mode, source) && pipeline.Rate() == 48000,
                "pipeline exposes final format for every source count and matrix");
        const auto actual = PipelineFloat(pipeline);
        Require(actual.size() == expected.size(), "matrix keeps exact frame length");
        for (std::size_t i = 0; i < actual.size(); ++i) Close(actual[i], expected[i], "real pipeline matrix matches independent coefficients");
        Require(decoded.SeekFrame(0), "rewind custom PCM fixture");
        pipeline.ResetAfterSeek();
        // Use exactly the float implementation's operation order for the last
        // half-LSB of the quantizer reference, not the independent double sum.
        Quantizer quantizer(s, pipeline.Channels(), pipeline.Rate());
        Require(PipelinePcm(pipeline, s.bits, 73) == Packed(quantizer, actual, pipeline.Channels(), s.bits),
                "actual PCM pipeline selects the custom PDF/shaping quantizer");
    }
}
void LegacyDefaults(const std::array<std::filesystem::path, 8>& paths) {
    for (const auto& path : paths) for (const auto mode : {OutputMode::SourceChannels, OutputMode::Stereo}) {
        PlaybackSettings s;
        s.output_mode = mode;
        // These choices are dormant unless custom_quantizer is explicitly on.
        s.dither_pdf = DitherPdf::Gaussian;
        s.noise_shape = NoiseShape::Ath;
        Decoder reference, decoded;
        OpenFixture(reference, path, s);
        OpenFixture(decoded, path, s);
        Require(!decoded.NativeActive(), "ordinary fixtures retain libopusfile fast path");
        std::array<std::int16_t, 576 * 8> block{};
        std::vector<unsigned char> expected;
        for (;;) {
            const int n = reference.ReadFrames(block.data(), 576);
            Require(n >= 0, "legacy integer reference");
            if (!n) break;
            const auto* bytes = reinterpret_cast<const unsigned char*>(block.data());
            expected.insert(expected.end(), bytes, bytes + n * reference.OutputChannels() * 2);
        }
        PcmPipeline pipeline(decoded, s);
        Require(PipelinePcm(pipeline, 16) == expected, "default integer PCM is byte-exact libopusfile including its dither/soft clipping");
    }
    // Non-16-bit default conversion must still use the original TPDF stream,
    // not the new quantizer merely because a new PDF/shape preference is set.
    for (int bits : {8, 24, 32}) {
        PlaybackSettings s;
        s.bits = bits;
        s.dither_pdf = DitherPdf::Gaussian;
        s.noise_shape = NoiseShape::Ath;
        Decoder reference, decoded;
        OpenFixture(reference, paths[5], s);
        OpenFixture(decoded, paths[5], s);
        const auto floats = DecoderFloat(reference);
        std::vector<unsigned char> expected(floats.size() * (bits / 8));
        auto* out = expected.data();
        std::uint32_t rng = 0x9e3779b9;
        auto random = [&] {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return static_cast<double>(rng) / 4294967296.0;
        };
        const double scale = std::ldexp(1., bits - 1);
        for (float sample : floats) {
            double value = static_cast<double>(sample) * scale;
            if (sample != 0) {
                const double first = random();
                const double second = random();
                value += first - second;
            }
            const auto code = static_cast<std::int64_t>(std::llround(std::clamp(value, -scale, scale - 1)));
            const auto packed = static_cast<std::uint32_t>(bits == 8 ? code + 128 : code);
            for (int b = 0; b < bits / 8; ++b) *out++ = static_cast<unsigned char>(packed >> (8 * b));
        }
        PcmPipeline pipeline(decoded, s);
        Require(PipelinePcm(pipeline, bits, 113) == expected, "default conversion retains original TPDF and rounding");
    }
}
void FloatIsNotIntegerPcm(const std::filesystem::path& path) {
    PlaybackSettings a, b;
    a.output_mode = b.output_mode = OutputMode::SurroundII;
    a.bits = 8; a.custom_quantizer = true; a.dither_pdf = DitherPdf::Gaussian; a.noise_shape = NoiseShape::Ath;
    b.bits = 32; b.dither = false;
    Decoder first, second;
    OpenFixture(first, path, a); OpenFixture(second, path, b);
    PcmPipeline x(first, a), y(second, b);
    const auto floats = PipelineFloat(x);
    Require(floats == PipelineFloat(y), "float path ignores bit depth, dither PDF, shaping and custom packer");
    Require(std::any_of(floats.begin(), floats.end(), [](float f) { return std::abs(f) > 1.f; }),
            "float path preserves valid over-full-scale matrix/headroom values");
    Require(first.SeekFrame(0) && second.SeekFrame(0), "float packer state fixture rewind");
    x.ResetAfterSeek(); y.ResetAfterSeek();
    std::array<float, 576 * 8> skip{}, expected_float{};
    Require(x.ReadFloat(skip.data(), 113) == 113 && y.ReadFloat(skip.data(), 113) == 113,
            "consume float-only prefix");
    const int n = y.ReadFloat(expected_float.data(), 73);
    Require(n == 73, "read post-prefix reference");
    Quantizer fresh(a, x.Channels(), x.Rate());
    std::array<unsigned char, 576 * 8 * 4> expected{}, actual{};
    fresh.Pack(expected.data(), expected_float.data(), n);
    Require(x.Read(actual.data(), n) == n && std::equal(actual.begin(), actual.begin() + n * x.Channels(), expected.begin()),
            "ReadFloat never advances quantizer RNG or feedback histories");
    const auto position = x.PositionFrames();
    Require(x.ReadFloat(nullptr, 1) == OP_EINVAL && x.ReadFloat(skip.data(), 0) == OP_EINVAL &&
            x.ReadFloat(skip.data(), 577) == OP_EINVAL && x.Read(nullptr, 1) == OP_EINVAL &&
            x.Read(actual.data(), 577) == OP_EINVAL && x.PositionFrames() == position,
            "invalid bounded reads leave decoder and quantizer state unchanged");
}
void ResamplingAndSeeks(const std::array<std::filesystem::path, 8>& paths) {
    constexpr int frames = 5761, seek = 1234;
    for (int source : {1, 6, 8}) for (const auto mode : modes)
        for (int rate : {8000, 22050, 37800, 44100, 48000, 96000, 192000}) {
            PlaybackSettings s;
            s.output_mode = mode; s.sample_rate = rate; s.custom_quantizer = true;
            s.bits = 24; s.dither_pdf = DitherPdf::Triangular; s.noise_shape = NoiseShape::Ath;
            Decoder decoder;
            OpenFixture(decoder, paths[source - 1], s);
            PcmPipeline pipeline(decoder, s);
            Require(pipeline.Ready() && pipeline.Rate() == rate, "initialize final-channel resampler");
            const auto expected = static_cast<std::int64_t>(frames) * rate / 48000;
            Require(PipelineFloat(pipeline).size() == static_cast<std::size_t>(expected * pipeline.Channels()),
                    "all channel matrices preserve exact resampler drain length");
            Require(decoder.SeekFrame(seek), "sample-exact resampler seek");
            pipeline.ResetAfterSeek();
            Require(pipeline.PositionFrames() == seek, "seek resets output timeline and latency");
            const auto remaining = static_cast<std::int64_t>(frames - seek) * rate / 48000;
            Require(PipelinePcm(pipeline, s.bits, 37).size() == static_cast<std::size_t>(remaining * pipeline.Channels() * 3),
                    "integer and float paths share exact seek/tail rendering");
            Require(decoder.SeekFrame(frames), "seek to exact EOF");
            pipeline.ResetAfterSeek();
            Require(PipelineFloat(pipeline).empty() && pipeline.PositionFrames() == frames, "empty drain after EOF seek");
        }
}
void RenderedAmbisonicsRemainSpeakers(const std::filesystem::path& directory) {
    const auto ordinary = directory / L"coefficients-source.opus";
    const auto ambisonic = directory / L"rendered-ambisonic.opus";
    MakeOpus(ordinary, 6, 5761, 62410);
    // The same six coded channels are now explicitly declared as a legal
    // first-order ACN/SN3D field + non-diegetic stereo. Update the real Ogg CRC.
    std::ifstream input(ordinary, std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Require(bytes.size() > 64 && bytes[26] > 0, "read family-2 fixture source");
    const int header = 27 + bytes[26];
    int body = 0;
    for (int i = 27; i < header; ++i) body += bytes[i];
    Require(header + body <= static_cast<int>(bytes.size()) && std::memcmp(bytes.data() + header, "OpusHead", 8) == 0,
            "locate bounded first OpusHead page");
    bytes[header + 18] = 2;
    ogg_page page{bytes.data(), header, bytes.data() + header, body};
    ogg_page_checksum_set(&page);
    std::ofstream output(ambisonic, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    for (const auto render : {AmbisonicMode::Surround51, AmbisonicMode::Surround71}) {
        PlaybackSettings source, legacy;
        source.ambisonic = legacy.ambisonic = render;
        legacy.output_mode = OutputMode::LegacyUnmapped;
        Decoder a, b;
        OpenFixture(a, ambisonic, source); OpenFixture(b, ambisonic, legacy);
        Require(a.NativeActive() && b.NativeActive() && a.Info().mapping_family == 2, "real native Ambisonic rendering fixture");
        PcmPipeline expected(a, source), actual(b, legacy);
        Require(expected.Channels() == (render == AmbisonicMode::Surround51 ? 6 : 8), "Ambisonic renderer negotiates speakers");
        Require(PipelineFloat(expected) == PipelineFloat(actual), "legacy raw mode NEVER permutes already-rendered Ambisonic speakers");
    }
}
void GainOnFirstChainFrame(const std::filesystem::path& directory) {
    const auto first = directory / L"gain-first.opus", second = directory / L"gain-second.opus";
    const auto chain = directory / L"gain-chain.opus";
    constexpr int first_frames = 481, second_frames = 1001;
    MakeOpus(first, 2, first_frames, 62421,
             {"R128_TRACK_GAIN=-256", "REPLAYGAIN_TRACK_PEAK=0.25"}, 256);
    MakeOpus(second, 2, second_frames, 62422,
             {"R128_TRACK_GAIN=-2048", "REPLAYGAIN_TRACK_PEAK=2"}, 3 * 256);
    std::filesystem::copy_file(first, chain, std::filesystem::copy_options::overwrite_existing);
    AppendFile(chain, second);
    for (bool native : {false, true}) {
        PlaybackSettings raw, normalized;
        raw.gain_mode = GainMode::Raw;
        raw.ignore_extensions = normalized.ignore_extensions = native;
        normalized.gain_mode = GainMode::Track;
        normalized.target_lufs = -18;
        normalized.pre_gain_q8 = -2 * 256;
        normalized.peak_mode = 1;
        normalized.prevent_clipping = true;
        Decoder reference, decoded;
        OpenFixture(reference, chain, raw); OpenFixture(decoded, chain, normalized);
        Require(decoded.NativeActive() == native, "cross-link gain regression selects each decoder backend");
        const auto samples = DecoderFloat(reference), actual = DecoderFloat(decoded);
        Require(actual.size() == samples.size() && actual.size() == (first_frames + second_frames) * 2,
                "gain-changing same-layout chain retains every frame");
        const double gain_first = std::pow(10., 3. / 20.);
        const double second_q8 = std::floor(-20. * 256 * std::log10(2.));
        const double gain_second = std::pow(10., second_q8 / (20. * 256));
        for (std::size_t i = 0; i < actual.size(); ++i)
            Close(actual[i], samples[i] * (i < first_frames * 2 ? gain_first : gain_second),
                  "each decoder backend applies new-link gain to its very first frame");
        // Seeking updates link metadata before the decode callback runs. The
        // same absolute policy must therefore also be reapplied by SeekFrame.
        for (const int target : {0, first_frames}) {
            decoded.SetGain(normalized, false);
            Require(decoded.SeekFrame(target), "seek between gain-changing logical links");
            std::array<float, 113 * 2> after_seek{};
            Require(decoded.ReadFloatFrames(after_seek.data(), 113) == 113, "decode gain after cross-link seek");
            const double multiplier = target == 0 ? gain_first : gain_second;
            for (std::size_t i = 0; i < after_seek.size(); ++i)
                Close(after_seek[i], samples[static_cast<std::size_t>(target) * 2 + i] * multiplier,
                      "first frame after cross-link seek uses target-link absolute gain");
        }
    }
}
void RealPipelineTests(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::array<std::filesystem::path, 8> paths;
    for (int c = 1; c <= 8; ++c) {
        paths[c - 1] = directory / (L"processing-" + std::to_wstring(c) + L".opus");
        MakeOpus(paths[c - 1], c, 5761, 62400 + c);
    }
    LegacyDefaults(paths);
    RealMatricesAndQuantizer(paths);
    ResamplingAndSeeks(paths);
    const auto loud = directory / L"processing-headroom.opus";
    MakeOpus(loud, 6, 5761, 62409, {}, 24 * 256);
    FloatIsNotIntegerPcm(loud);
    RenderedAmbisonicsRemainSpeakers(directory);
    GainOnFirstChainFrame(directory);
}
#endif
} // namespace

#ifdef WINAMP_OPUS_PROCESSING_UNIT_ONLY
int main() {
#else
int wmain(int argc, wchar_t** argv) {
#endif
    try {
        PackingAndBounds();
        DeterminismAndChunking();
        ChannelIndependence();
        DistributionStatistics();
        RatePoliciesAndOverload();
        ChannelMatrices();
        ShapingSpectrum();
#ifndef WINAMP_OPUS_PROCESSING_UNIT_ONLY
        Require(argc == 2, "usage: processing_feature_tests DATA_DIR");
        RealPipelineTests(argv[1]);
        std::cout << "Real PCM pipeline: 1..8-channel modes, byte-exact defaults, float-only rendering, custom quantization, "
                     "resampler drain/seek, rendered Ambisonics and chained gain passed.\n";
#endif
        std::cout << "Processing: PCM8/16/24/32, distributions, deterministic state, channel isolation, "
                     "ATH policy/spectra, overloads and all 1..8-channel matrices passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
