#include "Service.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <type_traits>

#include <otter/Analysis/Provider/AnalysisInput.h>
#include <stdcorelib/support/logging.h>

#include <dsspextraction/ExtractorCatalog.h>
#include <dsspextraction/Results.h>
#include <dsspextraction/Slicing.h>
#include <dsspextraction/Separation.h>
#include <dsspextraction/Tempo.h>
#include <dsspmedia/Wave.h>
#include <dsspresource/TaskExecutor.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::extraction {
    namespace {
        namespace Note = otter::Api::Note::L1;
        namespace F0 = otter::Api::F0::L1;
        stdc::LogCategory log("dssp.extraction.Service");

        template <class Output>
        struct Request {
            Request(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, std::stop_token externalStop, localization::Context locale, Completion<Output> complete)
                : catalog(std::move(catalog)), record(record), tasks(std::move(tasks)), locale(std::move(locale)), complete(std::move(complete)), externalCancel(externalStop, [this] { stop.request_stop(); }) {}

            std::shared_ptr<const ExtractorCatalog> catalog;
            const ExtractorRecord &record;
            std::shared_ptr<resource::TaskExecutor> tasks;
            const localization::Context locale;
            Completion<Output> complete;
            resource::TaskProgress progress;
            std::stop_source stop;
            std::stop_callback<std::function<void()>> externalCancel;
            std::atomic_flag done = ATOMIC_FLAG_INIT;
            std::vector<float> samples;
            std::vector<SampleSpan> spans;

            void finish(ExtractionResult<Output> result) {
                if (done.test_and_set(std::memory_order_acq_rel)) return;
                if (stop.stop_requested()) result = fail<ExtractionErrors>(CancelledError(locale.text("extraction.extraction_request_cancelled_for_extractor", "Extraction request was canceled for extractor {0}", record.id)));
                complete(std::move(result));
            }

            void failExecution(const inference::ExecutionErrors &errors) {
                if (errors.get_if<inference::CancelledError>()) {
                    finish(fail<ExtractionErrors>(CancelledError(locale.text("extraction.extraction_cancelled", "Extraction was canceled for extractor {0}", record.id), errors.asError().clone())));
                } else {
                    finish(fail<ExtractionErrors>(OperationError(locale.text("extraction.extraction_inference_failed", "Extraction inference failed for extractor {0}", record.id), errors.asError().clone())));
                }
            }
        };

        template <class Output>
        drogon::Task<bool> background(std::shared_ptr<Request<Output>> request) {
            auto ready = co_await request->tasks->resume(request->stop.get_token());
            if (!ready) {
                if (ready.error().template get_if<resource::TaskCancelledError>()) request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.extraction_background_cancelled", "Extraction background task was canceled"))));
                else request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.extraction_background_failed", "Cannot execute extraction background task"), ready.error().asError().clone())));
                co_return false;
            }
            if (request->stop.stop_requested()) {
                request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.extraction_request_cancelled", "Extraction request was canceled"))));
                co_return false;
            }

            co_return true;
        }

        template <class Output>
        bool prepare(const std::shared_ptr<Request<Output>> &request, std::string_view url, double threshold) {
            auto decoded = media::readWaveDataUrl(url, request->stop.get_token(), request->locale);
            if (request->stop.stop_requested()) {
                request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.extraction_audio_decoding_cancelled", "Audio decoding was canceled"))));
                return false;
            }
            if (!decoded) {
                if (decoded.error().template get_if<media::InvalidMediaError>()) {
                    request->finish(fail<ExtractionErrors>(InvalidInputError(request->locale.text("extraction.extractor_audio_invalid", "Invalid audio for extractor {0}: {1}", request->record.id, decoded.error().asError().message()), decoded.error().asError().clone())));
                } else {
                    request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.extractor_audio_decode_failed", "Cannot decode audio for extractor {0}", request->record.id), decoded.error().asError().clone())));
                }
                return false;
            }
            if (decoded->sampleRate != request->record.sampleRate) {
                request->finish(fail<ExtractionErrors>(InvalidInputError(request->locale.text("extraction.extractor_sample_rate_mismatch", "Extractor {0} requires {1} Hz audio; received {2} Hz", request->record.id, std::to_string(request->record.sampleRate), std::to_string(decoded->sampleRate)))));
                return false;
            }

            otter::Api::Common::L1::AudioSegment audio;
            audio.sampleRate = decoded->sampleRate;
            audio.channelCount = decoded->channelCount;
            audio.samples = std::move(decoded->samples);
            auto mono = inference::nativeCall("Otter prepareSamples", [&] { return otter::prepareSamples(audio, request->record.sampleRate, 1, 0); });
            if (!mono) {
                request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.extractor_audio_prepare_failed", "Cannot prepare audio for extractor {0}", request->record.id), mono.error().clone())));
                return false;
            }
            if (std::ranges::any_of(*mono, [](float sample) { return !std::isfinite(sample); })) {
                request->finish(fail<ExtractionErrors>(InvalidInputError(request->locale.text("extraction.extractor_channel_average_nonfinite", "Audio channel averaging produced a nonfinite sample for extractor {0}", request->record.id))));
                return false;
            }

            // PreparedSamples borrows mono audio and owns downmixed audio. Consume
            // its view before audio is destroyed or its sample buffer is moved.
            if (audio.channelCount == 1) {
                request->samples = std::move(audio.samples);
            } else {
                request->samples.assign(mono->begin(), mono->end());
            }

            auto spans = sliceAudio(request->samples, request->record.sampleRate, request->record.maximumDuration, request->record.outputInterval, threshold, request->stop.get_token());
            if (!spans) {
                if (spans.error().template get_if<CancelledError>()) {
                    request->finish(fail<ExtractionErrors>(CancelledError(request->locale.text("extraction.extractor_audio_slicing_cancelled", "Audio slicing was canceled for extractor {0}", request->record.id), spans.error().asError().clone())));
                } else {
                    request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.extractor_audio_partition_failed", "Cannot partition audio for extractor {0}", request->record.id), spans.error().asError().clone())));
                }
                return false;
            }

            request->spans = std::move(*spans);
            log.stdcTrace("Prepared extraction extractor=%1 pcm_frames=%2 sample_rate=%3 spans=%4", request->record.id, request->samples.size(), request->record.sampleRate, request->spans.size());
            return true;
        }

        template <class Executive, class InputType, class ResultType, class Output, class Convert>
        void extract(std::shared_ptr<const ExtractorCatalog> catalog, const ExtractorRecord &record, std::shared_ptr<resource::TaskExecutor> tasks, Input input, double threshold, std::stop_token stop, Completion<Output> complete, Convert convert) {
            const auto request = std::make_shared<Request<Output>>(std::move(catalog), record, std::move(tasks), stop, std::move(input.locale), std::move(complete));
            request->progress = input.progress;
            drogon::async_run([request, input = std::move(input), threshold, convert = std::move(convert)]() mutable -> drogon::Task<> {
                if (!(co_await background(request))) co_return;
                if (!prepare(request, input.audioUrl, threshold)) co_return;
                input.audioUrl.clear();
                inference::ExecutionResult<std::vector<std::unique_ptr<ResultType>>> result{std::vector<std::unique_ptr<ResultType>>{}};
                if (!request->spans.empty()) result = co_await request->record.pool->template useCoro<Executive>(request->record.resource, request->stop.get_token(), [request](Executive &executive, std::stop_token token) -> inference::ExecutionResult<std::vector<std::unique_ptr<ResultType>>> {
                    std::vector<std::unique_ptr<ResultType>> results;
                    results.reserve(request->spans.size());
                    std::size_t totalFrames = 0;
                    for (const auto &span : request->spans) totalFrames += span.end - span.begin;
                    std::size_t completedFrames = 0;
                    for (std::size_t index = 0; index < request->spans.size(); ++index) {
                        if (token.stop_requested() || request->stop.stop_requested()) return fail<inference::ExecutionErrors>(inference::CancelledError("Extraction was cancelled between spans"));
                        const auto &span = request->spans[index];
                        InputType native;
                        native.audio.sampleRate = request->record.sampleRate;
                        native.audio.channelCount = 1;
                        native.audio.startTime = static_cast<double>(span.begin) / request->record.sampleRate;
                        native.audio.samples.assign(request->samples.begin() + span.begin, request->samples.begin() + span.end);
                        const auto length = span.end - span.begin;
                        native.progress = [request, completedFrames, length, totalFrames, token](double value) {
                            if (token.stop_requested() || request->stop.stop_requested()) return false;
                            if constexpr (std::is_same_v<ResultType, Note::NoteResult>) {
                                // Complete spans require successful result validation.
                                if (std::isfinite(value) && value >= 0 && value < 1) request->progress.advance((completedFrames + length * value) / totalFrames);
                            }
                            return true;
                        };
                        // Language, knownNotes and all knobs retain Otter defaults.
                        log.stdcTrace("Running extraction extractor=%1 span=%2/%3 start_seconds=%4 pcm_frames=%5", request->record.id, index + 1, request->spans.size(), native.audio.startTime, span.end - span.begin);
                        auto result = inference::nativeCall("Otter analyzer::start", [&] { return executive.start(native); });
                        if (!result) return fail<inference::ExecutionErrors>(inference::ExecutionError("Extractor " + request->record.id + " failed in span " + std::to_string(index), result.error().clone()));
                        if (!*result) return fail<inference::ExecutionErrors>(inference::ExecutionError("Extractor " + request->record.id + " returned a null result in span " + std::to_string(index)));
                        if (executive.state() == srt::ITask::Canceled) return fail<inference::ExecutionErrors>(inference::CancelledError("Otter analyzer was cancelled"));
                        const auto start = static_cast<double>(span.begin) / request->record.sampleRate;
                        const auto end = static_cast<double>(span.end) / request->record.sampleRate;
                        if constexpr (std::is_same_v<ResultType, Note::NoteResult>) {
                            for (const auto &note : (*result)->notes) {
                                if (note.start < start - 1e-5 || note.start + note.duration > end + 1e-5) return fail<inference::ExecutionErrors>(inference::ExecutionError("Note timing lies outside its input span"));
                            }
                            auto valid = noteOutput((*result)->notes, end);
                            if (!valid) return fail<inference::ExecutionErrors>(inference::ExecutionError("Invalid note extraction result", valid.error().clone()));
                        } else {
                            PitchBuilder validation(request->record.outputInterval);
                            auto valid = validation.append(**result, start, end, request->record.sampleRate);
                            if (!valid) return fail<inference::ExecutionErrors>(inference::ExecutionError("Invalid pitch extraction result", valid.error().clone()));
                        }
                        results.push_back(std::move(*result));
                        completedFrames += length;
                        request->progress.advance(static_cast<double>(completedFrames) / totalFrames);
                    }

                    return results;
                }, request->progress);
                if (!(co_await background(request))) co_return;
                if (!result) {
                    request->failExecution(result.error());
                    co_return;
                }

                auto output = convert(*request, *result);
                if (!output) request->finish(fail<ExtractionErrors>(OperationError(request->locale.text("extraction.extractor_model_result_invalid", "Invalid model result from extractor {0}", request->record.id), output.error().clone())));
                else request->finish(std::move(*output));
            });
        }
    }

    struct Service::Impl {
        std::shared_ptr<const ExtractorCatalog> catalog;
        std::shared_ptr<resource::TaskExecutor> tasks;
        double threshold = 0.02;
    };

    std::expected<std::shared_ptr<Service>, inference::InitializationError> Service::create(std::shared_ptr<inference::Environment> environment, double threshold) {
        if (!std::isfinite(threshold) || threshold < 0) return std::unexpected{inference::InitializationError("Invalid extraction silence threshold")};
        auto catalog = ExtractorCatalog::discover(std::move(environment));
        if (!catalog) return std::unexpected{std::move(catalog.error())};
        auto tasks = resource::TaskExecutor::create("dssp-extraction-tasks");
        if (!tasks) return std::unexpected{inference::InitializationError("Cannot create extraction background workers", tasks.error().clone())};
        auto impl = std::make_unique<Impl>();
        impl->catalog = std::move(*catalog);
        impl->tasks = std::move(*tasks);
        impl->threshold = threshold;
        return std::shared_ptr<Service>(new Service(std::move(impl)));
    }

    Service::Service(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Service::~Service() = default;
    void Service::shutdown() { m_impl->tasks->shutdown(); }

    void Service::tempo(Input input, std::stop_token stop, Completion<TempoOutput> complete) const {
        const auto *record = m_impl->catalog->find(Kind::Tempo, input.extractor);
        if (!record) {
            complete(fail<ExtractionErrors>(InvalidInputError(input.locale.text("extraction.tempo_extractor_not_found", "Tempo extractor does not exist: {0}", input.extractor))));
            return;
        }

        extractTempo(m_impl->catalog, *record, m_impl->tasks, std::move(input), stop, std::move(complete));
    }

    CatalogResult<ExtractorList> Service::extractors(const localization::Context &locale) const { return m_impl->catalog->list(locale); }

    CatalogResult<ExtractorInfo> Service::extractor(Kind kind, std::string_view id, const localization::Context &locale) const { return m_impl->catalog->metadata(kind, id, locale); }

    void Service::note(Input input, std::stop_token stop, Completion<NoteOutput> complete) const {
        const auto *record = m_impl->catalog->find(Kind::Note, input.extractor);
        if (!record) {
            complete(fail<ExtractionErrors>(InvalidInputError(input.locale.text("extraction.note_extractor_not_found", "Note extractor does not exist: {0}", input.extractor))));
            return;
        }

        extract<Note::NoteExecutive, Note::NoteStartInput, Note::NoteResult>(m_impl->catalog, *record, m_impl->tasks, std::move(input), m_impl->threshold, stop, std::move(complete), [](const auto &request, const auto &results) -> std::expected<NoteOutput, OperationError> {
            std::vector<Note::NoteInfo> notes;
            for (std::size_t index = 0; index < results.size(); ++index) {
                const auto start = static_cast<double>(request.spans[index].begin) / request.record.sampleRate;
                const auto end = static_cast<double>(request.spans[index].end) / request.record.sampleRate;
                for (const auto &note : results[index]->notes) {
                    if (note.start < start - 1e-5 || note.start + note.duration > end + 1e-5) return std::unexpected{OperationError("Note timing lies outside input span " + std::to_string(index))};
                    notes.push_back(note);
                }
            }

            return noteOutput(std::move(notes), static_cast<double>(request.samples.size()) / request.record.sampleRate);
        });
    }

    void Service::pitch(Input input, std::stop_token stop, Completion<PitchOutput> complete) const {
        const auto *record = m_impl->catalog->find(Kind::Pitch, input.extractor);
        if (!record) {
            complete(fail<ExtractionErrors>(InvalidInputError(input.locale.text("extraction.pitch_extractor_not_found", "Pitch extractor does not exist: {0}", input.extractor))));
            return;
        }

        extract<F0::F0Executive, F0::F0StartInput, F0::F0Result>(m_impl->catalog, *record, m_impl->tasks, std::move(input), m_impl->threshold, stop, std::move(complete), [](const auto &request, const auto &results) -> std::expected<PitchOutput, OperationError> {
            PitchBuilder builder(request.record.outputInterval);
            for (std::size_t index = 0; index < results.size(); ++index) {
                const auto start = static_cast<double>(request.spans[index].begin) / request.record.sampleRate;
                const auto end = static_cast<double>(request.spans[index].end) / request.record.sampleRate;
                auto appended = builder.append(*results[index], start, end, request.record.sampleRate);
                if (!appended) return std::unexpected{std::move(appended.error())};
            }

            return builder.take();
        });
    }

    void Service::separation(SeparationInput input, std::stop_token stop, Completion<SeparationOutput> complete) const {
        const auto *record = m_impl->catalog->find(Kind::Separation, input.extractor);
        if (!record) {
            complete(fail<ExtractionErrors>(InvalidInputError(input.locale.text("extraction.separation_extractor_not_found", "Separation extractor does not exist: {0}", input.extractor))));
            return;
        }

        extractSeparation(m_impl->catalog, *record, m_impl->tasks, std::move(input), stop, std::move(complete));
    }
}
