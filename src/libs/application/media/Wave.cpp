#include "Wave.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include <dr_wav.h>
#include <openssl/evp.h>
#include <stdcorelib/scope_guard.h>
#include <uriparser/Uri.h>

#include <dsspcodecutil/codecutil.h>

namespace dssp::media {
    namespace {
        std::expected<std::string, MediaInputErrors> waveBytes(std::string_view url, const localization::Context &locale) {
            UriUriA uri{};
            const auto cleanup = stdc::make_scope_guard([&] { uriFreeUriMembersA(&uri); });
            const auto parsed = uriParseSingleUriExA(&uri, url.data(), url.data() + url.size(), nullptr);
            if (parsed == URI_ERROR_SYNTAX) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_url_invalid", "Audio URL is not a valid absolute URI")));
            }
            if (parsed != URI_SUCCESS) {
                return fail<MediaInputErrors>(MediaError("Cannot parse audio URI", std::make_shared<const ExternalError>("URI library failed", NativeDiagnostic{"uriparser", "uriParseSingleUriExA", "UriErrorCode", parsed})));
            }

            const auto comma = url.find(',');
            if (comma == std::string_view::npos || uri.hostText.first || uri.query.first || uri.fragment.first) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_data_url_required", "Audio requires a WAV base64 data URL")));
            }

            std::string header(url.substr(0, comma));
            const auto headerEnd = uriUnescapeInPlaceA(header.data());
            header.resize(static_cast<std::size_t>(headerEnd - header.data()));
            std::ranges::transform(header, header.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (header != "data:audio/wav;base64" && header != "data:audio/x-wav;base64") {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_data_url_components_invalid", "Audio requires data scheme, audio/wav or audio/x-wav MIME, and base64 encoding")));
            }

            std::string encoded(url.substr(comma + 1));
            const auto end = uriUnescapeInPlaceA(encoded.data());
            encoded.resize(static_cast<std::size_t>(end - encoded.data()));
            if (encoded.empty() || encoded.size() % 4 || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_base64_length_invalid", "Audio data has an invalid base64 length")));
            }

            std::string bytes(encoded.size() / 4 * 3, '\0');
            const auto count = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()), reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<int>(encoded.size()));
            const auto padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0;
            if (count < padding) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_base64_invalid", "Audio data is not valid base64")));
            }

            bytes.resize(static_cast<std::size_t>(count - padding));
            // Canonical re-encoding catches misplaced padding, ignored whitespace
            // and nonzero padding bits without implementing another base64 codec.
            const codecutil::Base64EncodingView canonical{std::span(bytes)};
            if (!std::ranges::equal(canonical, encoded)) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_base64_noncanonical", "Audio data is not canonical base64")));
            }

            return bytes;
        }
    }

    std::expected<WaveAudio, MediaInputErrors> readWaveDataUrl(std::string_view url, std::stop_token stop, const localization::Context &locale) {
        auto bytes = waveBytes(url, locale);
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }

        drwav reader{};
        if (!drwav_init_memory(&reader, bytes->data(), bytes->size(), nullptr)) {
            return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_unreadable", "Audio data is not a readable WAV container")));
        }

        const auto cleanup = stdc::make_scope_guard([&] { drwav_uninit(&reader); });
        const bool pcm = reader.translatedFormatTag == DR_WAVE_FORMAT_PCM && (reader.bitsPerSample == 8 || reader.bitsPerSample == 16 || reader.bitsPerSample == 24 || reader.bitsPerSample == 32);
        const bool floating = reader.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT && (reader.bitsPerSample == 32 || reader.bitsPerSample == 64);
        if (reader.container != drwav_container_riff || (!pcm && !floating) || !reader.channels || !reader.sampleRate || reader.channels > static_cast<unsigned>(std::numeric_limits<int>::max()) || reader.sampleRate > static_cast<unsigned>(std::numeric_limits<int>::max())) {
            return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_format_unsupported", "Audio WAV has an unsupported container, encoding, channel count or sample rate")));
        }

        const auto frameBytes = static_cast<std::uint64_t>(reader.channels) * (reader.bitsPerSample / 8);
        if (!reader.totalPCMFrameCount || reader.totalPCMFrameCount > std::vector<float>().max_size() / reader.channels) {
            return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_frame_count_invalid", "Audio WAV has an empty or unrepresentable frame count")));
        }
        if (reader.dataChunkDataSize % frameBytes || reader.totalPCMFrameCount > bytes->size() / frameBytes) {
            return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_frames_truncated_or_misaligned", "Audio WAV frame data is truncated or misaligned")));
        }

        WaveAudio output;
        output.sampleRate = static_cast<int>(reader.sampleRate);
        output.channelCount = static_cast<int>(reader.channels);
        output.samples.resize(static_cast<std::size_t>(reader.totalPCMFrameCount) * reader.channels);
        drwav_uint64 read = 0;
        while (read < reader.totalPCMFrameCount && !stop.stop_requested()) {
            const auto frames = std::min<drwav_uint64>(reader.totalPCMFrameCount - read, 65536);
            const auto count = drwav_read_pcm_frames_f32(&reader, frames, output.samples.data() + static_cast<std::size_t>(read) * reader.channels);
            if (count != frames) {
                return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_frames_truncated", "Audio WAV frame data is truncated")));
            }
            read += count;
        }

        if (std::ranges::any_of(output.samples, [](float sample) { return !std::isfinite(sample); })) {
            return fail<MediaInputErrors>(InvalidMediaError(locale.text("media.audio_wav_samples_nonfinite", "Audio WAV contains nonfinite samples")));
        }

        return output;
    }

    std::expected<std::string, MediaError> waveDataUrl(std::span<const std::uint8_t> pcm, int sampleRate, double duration) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        const double count = std::round(duration * sampleRate);
        if (sampleRate <= 0 || !std::isfinite(duration) || duration < 0 || !std::isfinite(count) || count > maximumWaveSampleCount || pcm.size() % sizeof(float)) {
            return std::unexpected{MediaError("PCM size, sample rate or duration cannot be represented by an IEEE float32 RIFF WAV")};
        }

        std::vector<float> samples(pcm.size() / sizeof(float));
        if (!pcm.empty()) {
            std::memcpy(samples.data(), pcm.data(), pcm.size());
        }
        if (std::ranges::any_of(samples, [](float value) { return !std::isfinite(value); })) {
            return std::unexpected{MediaError("Vocoder PCM contains a nonfinite sample")};
        }

        samples.resize(static_cast<std::size_t>(count), 0);
        const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 1, static_cast<drwav_uint32>(sampleRate), 32};
        void *bytes = nullptr;
        std::size_t size = 0;
        drwav writer{};
        if (!drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr)) {
            if (bytes) {
                drwav_free(bytes, nullptr);
            }
            return std::unexpected{MediaError("dr_wav could not initialize the in-memory WAV encoder")};
        }

        const auto written = samples.empty() ? drwav_uint64{0} : drwav_write_pcm_frames(&writer, samples.size(), samples.data());
        const auto finished = drwav_uninit(&writer);
        const std::unique_ptr<void, decltype([](void *data) { drwav_free(data, nullptr); })> storage(bytes);
        if (written != samples.size() || finished != DRWAV_SUCCESS || !bytes || !size) {
            return std::unexpected{MediaError("dr_wav could not complete the in-memory WAV output")};
        }

        const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
        return "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
    }
}
