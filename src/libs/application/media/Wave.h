#ifndef DSSP_MEDIA_WAVE_H
#define DSSP_MEDIA_WAVE_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

#include <dsspmedia/MediaError.h>

namespace dssp::media {
    // Reserve 128 bytes for RIFF WAV metadata in float32 output.
    inline constexpr std::size_t maximumWaveSampleCount = (std::numeric_limits<std::uint32_t>::max() - 128) / sizeof(float);

    // Encode generated native float32 PCM in memory. No upload or output file.
    std::expected<std::string, MediaError> waveDataUrl(std::span<const std::uint8_t> pcm, int sampleRate, double duration);
    struct WaveAudio {
        int sampleRate = 0;
        int channelCount = 0;
        std::vector<float> samples;
    };

    // Decode the actual container in memory; no file access or resampling.
    std::expected<WaveAudio, MediaInputErrors> readWaveDataUrl(std::string_view, std::stop_token = {});
}

#endif // DSSP_MEDIA_WAVE_H
