#include "Separation.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include <dr_wav.h>
#include <stdcorelib/str.h>

#include <dsspcodecutil/codecutil.h>
#include <dsspmedia/Wave.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::extraction {
    namespace {
        namespace Sep = srtsepinfer::Api::Separation::L1;
        struct Request {
            Request(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, std::stop_token externalStop, Completion<SeparationOutput> complete)
                : catalog(std::move(catalog)), record(record), tasks(std::move(tasks)), complete(std::move(complete)), cancel(externalStop, [this] { stop.request_stop(); }) {}

            std::shared_ptr<const ExtractorCatalog> catalog;
            const ExtractorRecord &record;
            std::shared_ptr<resource::TaskExecutor> tasks;
            Completion<SeparationOutput> complete;
            resource::TaskProgress progress;
            std::stop_source stop;
            std::stop_callback<std::function<void()>> cancel;
            std::atomic_flag done = ATOMIC_FLAG_INIT;
            void finish(ExtractionResult<SeparationOutput> result) {
                if (done.test_and_set(std::memory_order_acq_rel))
                    return;
                if (stop.stop_requested())
                    result = fail<ExtractionErrors>(CancelledError("Audio separation was cancelled for extractor " + record.id));
                complete(std::move(result));
            }
        };

        drogon::Task<bool> background(std::shared_ptr<Request> request) {
            auto ready = co_await request->tasks->resume(request->stop.get_token());
            if (!ready) {
                if (ready.error().get_if<resource::TaskCancelledError>())
                    request->finish(fail<ExtractionErrors>(CancelledError("Audio separation background task was cancelled")));
                else
                    request->finish(fail<ExtractionErrors>(OperationError("Cannot run audio separation background task", ready.error().asError().clone())));
                co_return false;
            }
            if (request->stop.stop_requested()) {
                request->finish(fail<ExtractionErrors>(CancelledError("Audio separation was cancelled")));
                co_return false;
            }

            co_return true;
        }

        std::expected<std::string, OperationError> encode(const Sep::AudioSegment &audio) {
            const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, static_cast<drwav_uint32>(audio.channelCount), static_cast<drwav_uint32>(audio.sampleRate), 32};
            void *bytes = nullptr;
            std::size_t size = 0;
            drwav writer{};
            if (!drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr)) {
                if (bytes)
                    drwav_free(bytes, nullptr);
                return std::unexpected{OperationError("Cannot initialize separation WAV encoder")};
            }

            const auto frames = audio.samples.size() / audio.channelCount;
            const auto written = drwav_write_pcm_frames(&writer, frames, audio.samples.data());
            const auto finished = drwav_uninit(&writer);
            const std::unique_ptr<void, decltype([](void *data) { drwav_free(data, nullptr); })> storage(bytes);
            if (written != frames || finished != DRWAV_SUCCESS || !bytes || !size)
                return std::unexpected{OperationError("Cannot complete separation WAV output")};
            const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
            return "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
        }
    }

    void extractSeparation(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, SeparationInput input, std::stop_token stop, Completion<SeparationOutput> complete) {
        auto request = std::make_shared<Request>(std::move(catalog), record, std::move(tasks), stop, std::move(complete));
        request->progress = input.progress;
        drogon::async_run([request, input = std::move(input)]() mutable -> drogon::Task<> {
            if (!(co_await background(request)))
                co_return;
            if (!input.acceptableSchemes.empty() && std::ranges::none_of(input.acceptableSchemes, [](const auto &value) { return stdc::str::equals_insensitive(value, "data"); })) {
                request->finish(fail<ExtractionErrors>(InvalidInputError("acceptableSchemes does not accept the separated audio scheme data")));
                co_return;
            }
            if (!input.acceptableFormats.empty() && std::ranges::none_of(input.acceptableFormats, [](const auto &value) { return stdc::str::equals_insensitive(value, "audio/wav"); })) {
                request->finish(fail<ExtractionErrors>(InvalidInputError("acceptableFormats does not accept the separated audio format audio/wav")));
                co_return;
            }

            auto audio = media::readWaveDataUrl(input.audioUrl, request->stop.get_token());
            input.audioUrl.clear();
            if (request->stop.stop_requested()) {
                request->finish(fail<ExtractionErrors>(CancelledError("Separation decoding was cancelled")));
                co_return;
            }
            if (!audio) {
                if (audio.error().get_if<media::InvalidMediaError>())
                    request->finish(fail<ExtractionErrors>(InvalidInputError("Invalid separation audio: " + audio.error().asError().message(), audio.error().asError().clone())));
                else
                    request->finish(fail<ExtractionErrors>(OperationError("Cannot decode separation audio", audio.error().asError().clone())));
                co_return;
            }
            if (audio->sampleRate != request->record.sampleRate || audio->channelCount != request->record.channelCount) {
                request->finish(fail<ExtractionErrors>(InvalidInputError("Extractor " + request->record.id + " requires " + std::to_string(request->record.sampleRate) + " Hz, " + std::to_string(request->record.channelCount) + " channel audio")));
                co_return;
            }
            if (audio->samples.size() > media::maximumWaveSampleCount) {
                request->finish(fail<ExtractionErrors>(InvalidInputError("Separation audio exceeds the float32 RIFF WAV output limit")));
                co_return;
            }

            const auto expectedSamples = audio->samples.size();
            Sep::SeparationStartInput native;
            native.progress = [request](double value) {
                if (request->stop.stop_requested()) return false;
                request->progress.advance(value);
                return true;
            };
            native.audio = {audio->sampleRate, audio->channelCount, std::move(audio->samples), 0};
            auto result = co_await request->record.pool->useCoro<Sep::SeparationExecutive>(request->record.resource, request->stop.get_token(), [&native](Sep::SeparationExecutive &executive, std::stop_token token) -> inference::ExecutionResult<std::unique_ptr<Sep::SeparationResult>> {
                if (token.stop_requested())
                    return fail<inference::ExecutionErrors>(inference::CancelledError("Separation was cancelled before execution"));
                auto ran = inference::nativeCall("srtsepinfer SeparationExecutive::start", [&] { return executive.start(native); });
                if (executive.state() == srt::ITask::Canceled)
                    return fail<inference::ExecutionErrors>(inference::CancelledError("Separation inference was cancelled"));
                if (!ran)
                    return fail<inference::ExecutionErrors>(inference::ExecutionError("Separation inference failed", ran.error().clone()));
                if (!*ran)
                    return fail<inference::ExecutionErrors>(inference::ExecutionError("Separator returned no result"));
                return std::move(*ran);
            }, request->progress);
            if (!(co_await background(request)))
                co_return;
            if (!result) {
                if (result.error().get_if<inference::CancelledError>())
                    request->finish(fail<ExtractionErrors>(CancelledError("Separation inference was cancelled", result.error().asError().clone())));
                else
                    request->finish(fail<ExtractionErrors>(OperationError("Separation inference failed for " + request->record.id, result.error().asError().clone())));
                co_return;
            }
            if ((*result)->tracks.size() != request->record.tracks.size()) {
                request->finish(fail<ExtractionErrors>(OperationError("Separator returned an unexpected track count")));
                co_return;
            }

            SeparationOutput output;
            for (std::size_t i = 0; i < (*result)->tracks.size(); ++i) {
                if (request->stop.stop_requested()) {
                    request->finish(fail<ExtractionErrors>(CancelledError("Separation encoding was cancelled")));
                    co_return;
                }
                auto &track = (*result)->tracks[i];
                if (track.id != request->record.tracks[i].id || track.audio.sampleRate != request->record.sampleRate || track.audio.channelCount != request->record.channelCount || track.audio.startTime != 0 || track.audio.samples.size() != expectedSamples || std::ranges::any_of(track.audio.samples, [](float value) { return !std::isfinite(value); })) {
                    request->finish(fail<ExtractionErrors>(OperationError("Separated track violates the declared output contract")));
                    co_return;
                }
                auto url = encode(track.audio);
                if (!url) {
                    request->finish(fail<ExtractionErrors>(std::move(url.error())));
                    co_return;
                }
                output.tracks.push_back({std::move(track.id), std::move(*url)});
            }

            request->finish(std::move(output));
        });
    }
}
