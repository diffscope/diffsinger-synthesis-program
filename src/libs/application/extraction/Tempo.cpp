#include "Tempo.h"
#include <atomic>
#include <cmath>
#include <dsspmedia/Wave.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::extraction {
    namespace {
        namespace Beat = srtbeatinfer::Api::Beat::L1;
        struct Request {
            Request(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, std::stop_token externalStop, localization::Context locale, Completion<TempoOutput> complete)
                : catalog(std::move(catalog)), record(record), tasks(std::move(tasks)), locale(std::move(locale)), complete(std::move(complete)), cancel(externalStop, [this] { stop.request_stop(); }) {}

            std::shared_ptr<const ExtractorCatalog> catalog;
            const ExtractorRecord &record;
            std::shared_ptr<resource::TaskExecutor> tasks;
            const localization::Context locale;
            Completion<TempoOutput> complete;
            resource::TaskProgress progress;
            std::stop_source stop;
            std::stop_callback<std::function<void()>> cancel;
            std::atomic_flag done = ATOMIC_FLAG_INIT;
            void finish(ExtractionResult<TempoOutput> result) {
                if (done.test_and_set(std::memory_order_acq_rel)) return;
                if (stop.stop_requested()) result = fail<ExtractionErrors>(CancelledError(locale.text("extraction.tempo_request_cancelled_for_extractor", "Tempo extraction was canceled for {0}", record.id)));
                complete(std::move(result));
            }
        };

        drogon::Task<bool> background(std::shared_ptr<Request> request) {
            auto ready = co_await request->tasks->resume(request->stop.get_token());
            if (!ready) {
                if (ready.error().get_if<resource::TaskCancelledError>()) request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.tempo_background_cancelled", "Tempo background task was canceled"))));
                else request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.tempo_background_failed", "Cannot run tempo background task"), ready.error().asError().clone())));
                co_return false;
            }
            if (request->stop.stop_requested()) {
                request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.tempo_request_cancelled", "Tempo extraction was canceled"))));
                co_return false;
            }

            co_return true;
        }
    }

    void extractTempo(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, Input input, std::stop_token stop, Completion<TempoOutput> complete) {
        const auto request = std::make_shared<Request>(std::move(catalog), record, std::move(tasks), stop, std::move(input.locale), std::move(complete));
        request->progress = input.progress;
        drogon::async_run([request, input = std::move(input)]() mutable -> drogon::Task<> {
            if (!(co_await background(request))) co_return;
            auto audio = media::readWaveDataUrl(input.audioUrl, request->stop.get_token(), request->locale);
            input.audioUrl.clear();
            if (request->stop.stop_requested()) { request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.tempo_decoding_cancelled", "Tempo audio decoding was canceled")))); co_return; }
            if (!audio) {
                if (audio.error().get_if<media::InvalidMediaError>()) request->finish(fail<ExtractionErrors>(InvalidInputError(request->locale.text("extraction.tempo_audio_invalid", "Invalid tempo audio: {0}", audio.error().asError().message()), audio.error().asError().clone())));
                else request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.tempo_audio_decode_failed", "Cannot decode tempo audio"), audio.error().asError().clone())));
                co_return;
            }
            if (audio->sampleRate != request->record.sampleRate) {
                request->finish(fail<ExtractionErrors>(InvalidInputError(request->locale.text("extraction.tempo_sample_rate_mismatch", "Tempo extractor requires {0} Hz audio", std::to_string(request->record.sampleRate)))));
                co_return;
            }

            const auto frames = audio->samples.size() / audio->channelCount;
            const auto duration = static_cast<double>(frames) / audio->sampleRate;
            Beat::BeatStartInput native;
            native.progress = [request](double value) {
                if (request->stop.stop_requested()) return false;
                request->progress.advance(value);
                return true;
            };
            native.audio = {audio->sampleRate, 1, {}, 0};
            native.audio.samples.resize(frames);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                if ((frame & 65535) == 0 && request->stop.stop_requested()) { request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.tempo_mixing_cancelled", "Tempo channel mixing was canceled")))); co_return; }
                double value = 0;
                for (int channel = 0; channel < audio->channelCount; ++channel) value += audio->samples[frame * audio->channelCount + channel];
                native.audio.samples[frame] = static_cast<float>(value / audio->channelCount);
            }

            audio->samples.clear();
            // Beat tracking uses uninterrupted audio; silence slicing would lose measure context.
            auto result = co_await request->record.pool->useCoro<Beat::BeatExecutive>(request->record.resource, request->stop.get_token(), [&native](Beat::BeatExecutive &executive, std::stop_token token) -> inference::ExecutionResult<std::unique_ptr<Beat::BeatResult>> {
                if (token.stop_requested()) return fail<inference::ExecutionErrors>(inference::CancelledError("Tempo inference was cancelled before execution"));
                auto ran = inference::nativeCall("srtbeatinfer BeatExecutive::start", [&] { return executive.start(native); });
                if (executive.state() == srt::ITask::Canceled) return fail<inference::ExecutionErrors>(inference::CancelledError("Tempo inference was cancelled"));
                if (!ran) return fail<inference::ExecutionErrors>(inference::ExecutionError("Tempo inference failed", ran.error().clone()));
                if (!*ran) return fail<inference::ExecutionErrors>(inference::ExecutionError("Beat tracker returned no result"));
                return std::move(*ran);
            }, resource::TaskProgress([request](resource::TaskUpdate update) {
                if (update.state == resource::TaskUpdate::State::Queue) request->progress.queue();
            }));
            if (!(co_await background(request))) co_return;
            if (!result) {
                if (result.error().get_if<inference::CancelledError>()) request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.tempo_inference_cancelled", "Tempo inference was canceled"), result.error().asError().clone())));
                else request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.tempo_inference_failed", "Tempo inference failed for {0}", request->record.id), result.error().asError().clone())));
                co_return;
            }

            TempoOutput output;
            double previous = -1;
            for (const auto &beat : (*result)->beats) {
                if (!std::isfinite(beat.time) || beat.time < 0 || beat.time > duration || beat.time <= previous) {
                    request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.beat_timestamps_invalid", "Beat tracker returned invalid or unordered timestamps"))));
                    co_return;
                }
                output.beats.push_back({beat.time, beat.downbeat});
                previous = beat.time;
            }

            request->finish(std::move(output));
        });
    }
}
