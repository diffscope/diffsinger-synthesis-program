#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include <gtest/gtest.h>
#include <dr_wav.h>

#include <dsspcodecutil/codecutil.h>
#include <dsspmedia/Wave.h>

namespace dssp::tests {
    TEST(Wave, GeneratedAudioIsFloat32MonoAtTheActualRateAndHasTheRequestedDuration) {
        const std::array<float, 3> pcm{0.25f, -0.5f, 0.75f};
        const auto bytes = std::as_bytes(std::span(pcm));
        const auto url = media::waveDataUrl(std::span(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()), 48000, 5.0 / 48000);
        ASSERT_TRUE(url);
        ASSERT_TRUE(url->starts_with("data:audio/wav;base64,"));
        const auto encoded = std::string_view(*url).substr(22);
        const codecutil::Base64DecodingView decoded{encoded};
        const std::vector<unsigned char> wave(decoded.begin(), decoded.end());
        drwav reader{};
        ASSERT_TRUE(drwav_init_memory(&reader, wave.data(), wave.size(), nullptr));
        EXPECT_EQ(reader.sampleRate, 48000U);
        EXPECT_EQ(reader.channels, 1U);
        EXPECT_EQ(reader.bitsPerSample, 32U);
        EXPECT_EQ(reader.translatedFormatTag, DR_WAVE_FORMAT_IEEE_FLOAT);
        EXPECT_EQ(reader.totalPCMFrameCount, 5U);
        std::array<float, 5> samples{};
        EXPECT_EQ(drwav_read_pcm_frames_f32(&reader, samples.size(), samples.data()), 5U);
        EXPECT_EQ(samples, (std::array<float, 5>{0.25f, -0.5f, 0.75f, 0, 0}));
        EXPECT_EQ(drwav_uninit(&reader), DRWAV_SUCCESS);
    }

    TEST(Wave, ZeroDurationProducesAValidEmptyWaveAndInvalidNativeBytesFail) {
        const auto url = media::waveDataUrl({}, 44100, 0);
        ASSERT_TRUE(url);
        const auto encoded = std::string_view(*url).substr(22);
        const codecutil::Base64DecodingView decoded{encoded};
        const std::vector<unsigned char> wave(decoded.begin(), decoded.end());
        drwav reader{};
        ASSERT_TRUE(drwav_init_memory(&reader, wave.data(), wave.size(), nullptr));
        EXPECT_EQ(reader.totalPCMFrameCount, 0U);
        EXPECT_EQ(drwav_uninit(&reader), DRWAV_SUCCESS);
        const std::array<std::uint8_t, 1> invalid{0};
        EXPECT_FALSE(media::waveDataUrl(invalid, 44100, 1));
        EXPECT_FALSE(media::waveDataUrl({}, 0, 1));
        EXPECT_FALSE(media::waveDataUrl({}, 44100, 1e6));
        EXPECT_FALSE(media::waveDataUrl({}, 44100, std::numeric_limits<double>::max()));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(media::waveDataUrl(std::span(reinterpret_cast<const std::uint8_t *>(&nan), sizeof(nan)), 44100, 1));
    }

    TEST(Wave, InputAcceptsPercentEscapedBase64AndRejectsInvalidSchemasAndEmptyAudio) {
        const std::array<float, 2> samples{0.5f, -0.25f};
        const auto url = media::waveDataUrl(std::span(reinterpret_cast<const std::uint8_t *>(samples.data()), sizeof(samples)), 16000, 2.0 / 16000);
        ASSERT_TRUE(url);
        auto escaped = *url;
        std::string payload;
        for (const auto character : std::string_view(*url).substr(22)) {
            if (character == '+') payload += "%2B";
            else if (character == '/') payload += "%2F";
            else if (character == '=') payload += "%3D";
            else payload += character;
        }

        escaped = "data:audio/x-wav;base64," + payload;
        const auto result = media::readWaveDataUrl(escaped);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->sampleRate, 16000);
        EXPECT_EQ(result->samples, (std::vector<float>{0.5f, -0.25f}));
        EXPECT_FALSE(media::readWaveDataUrl("https://example.invalid/test.wav"));
        EXPECT_FALSE(media::readWaveDataUrl("data:audio/wav;base64,!!!!"));
        EXPECT_FALSE(media::readWaveDataUrl(*url + "#fragment"));
        EXPECT_FALSE(media::readWaveDataUrl(*url + "="));
        const auto empty = media::waveDataUrl({}, 16000, 0);
        ASSERT_TRUE(empty);
        EXPECT_FALSE(media::readWaveDataUrl(*empty));
    }

    TEST(Wave, InputDecodesPcm16Pcm24Pcm32AndFloat32WithoutResamplingOrDownmix) {
        for (const auto bits : {16U, 24U, 32U}) {
            // Little-endian 0.5 and -0.5 samples, one stereo frame.
            std::vector<std::uint8_t> samples(bits / 8 * 2, 0);
            samples[bits / 8 - 1] = 0x40;
            samples.back() = 0xc0;
            const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_PCM, 2, 44100, bits};
            void *bytes = nullptr;
            std::size_t size = 0;
            drwav writer{};
            ASSERT_TRUE(drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr));
            EXPECT_EQ(drwav_write_pcm_frames(&writer, 1, samples.data()), 1U);
            EXPECT_EQ(drwav_uninit(&writer), DRWAV_SUCCESS);
            const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
            const auto url = "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
            drwav_free(bytes, nullptr);
            const auto result = media::readWaveDataUrl(url);
            ASSERT_TRUE(result) << bits;
            EXPECT_EQ(result->sampleRate, 44100);
            EXPECT_EQ(result->channelCount, 2);
            ASSERT_EQ(result->samples.size(), 2U);
            EXPECT_FLOAT_EQ(result->samples[0], 0.5f);
            EXPECT_FLOAT_EQ(result->samples[1], -0.5f);
            const auto rawEncoded = std::string_view(url).substr(22);
            const codecutil::Base64DecodingView raw{rawEncoded};
            std::vector<std::uint8_t> truncated(raw.begin(), raw.end());
            truncated.resize(truncated.size() - samples.size());
            const codecutil::Base64EncodingView truncatedBase64{std::span(truncated)};
            EXPECT_FALSE(media::readWaveDataUrl("data:audio/wav;base64," + std::string(truncatedBase64.begin(), truncatedBase64.end())));
        }
    }
}
