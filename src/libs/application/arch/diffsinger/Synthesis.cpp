#include "Synthesis.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

#include <dsinfer/Api/Singers/DiffSinger/1/DiffSingerApiL1.h>
#include <stdcorelib/str.h>
#include <stdcorelib/support/logging.h>

#include <dssparchdiffsinger/SynthesisInferences.h>
#include <dssparchdiffsinger/SynthesisParameters.h>
#include <dsspmedia/Wave.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::arch_diffsinger {
    namespace {
        namespace Du = ds::Api::Duration::L1;
        namespace Pi = ds::Api::Pitch::L1;
        namespace Va = ds::Api::Variance::L1;
        namespace Ac = ds::Api::Acoustic::L1;
        namespace Vo = ds::Api::Vocoder::L1;
        stdc::LogCategory log("dssp.arch_diffsinger.Synthesis");

        arch::SynthesisResult<std::size_t> acousticFrames(const Ac::AcousticResult &result, int channels, const std::string &singer) {
            const auto invalid = [&](std::string message) -> arch::SynthesisResult<std::size_t> { return fail<arch::SynthesisErrors>(arch::OperationError(std::move(message) + " for singer " + singer)); };
            // Only tensor backend calls cross this exception boundary.
            try {
                if (!result.mel || !result.f0 || result.mel->dataType() != ds::ITensor::Float || result.f0->dataType() != ds::ITensor::Float) {
                    return invalid("Acoustic inference returned missing or incompatible tensors");
                }

                const auto shape = result.mel->shape();
                if (shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 || shape[2] != channels || static_cast<std::uint64_t>(shape[1]) > std::numeric_limits<std::size_t>::max() / sizeof(float) / static_cast<std::size_t>(channels)) {
                    return invalid("Acoustic mel tensor has an invalid shape");
                }

                const auto frames = static_cast<std::size_t>(shape[1]);
                const auto melValues = result.mel->view<float>();
                const auto f0Values = result.f0->view<float>();
                if (melValues.size() != frames * channels || f0Values.size() != frames || result.mel->byteSize() != melValues.size() * sizeof(float) || result.f0->byteSize() != frames * sizeof(float) || !melValues.data() || !f0Values.data() || std::ranges::any_of(melValues, [](float value) { return !std::isfinite(value); }) || std::ranges::any_of(f0Values, [](float value) { return !std::isfinite(value) || value < 0; })) {
                    return invalid("Acoustic tensor storage contains an invalid length or value");
                }
                if (result.f0->shape() != std::vector<std::int64_t>{1, shape[1]}) {
                    return invalid("Acoustic f0 tensor is not aligned with mel");
                }

                return frames;
            } catch (const std::exception &error) {
                return fail<arch::SynthesisErrors>(arch::OperationError("Cannot read acoustic tensors for singer " + singer, std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"dsinfer tensor backend", "validate acoustic tensors", "exception"})));
            }
        }

        template <class Output>
        class Request {
        public:
            Request(std::shared_ptr<const SingerCatalog> catalog, resource::TaskExecutor &tasks, std::stop_token stop, arch::Completion<Output> complete) : catalog(std::move(catalog)), tasks(tasks), stop(stop), m_complete(std::move(complete)) {}

            std::shared_ptr<const SingerCatalog> catalog;
            resource::TaskExecutor &tasks;
            std::stop_token stop;
            SynthesisContext context;
            SynthesisScore score;
            double duration = 0;
            resource::TaskProgress progress;

            bool cancelled() {
                if (!stop.stop_requested()) {
                    return false;
                }
                finish(fail<arch::SynthesisErrors>(arch::CancelledError("DiffSinger synthesis request was cancelled")));
                return true;
            }

            void finish(arch::SynthesisResult<Output> result) {
                if (m_done.test_and_set(std::memory_order_acq_rel)) {
                    return;
                }
                if (stop.stop_requested()) {
                    result = fail<arch::SynthesisErrors>(arch::CancelledError("DiffSinger synthesis request was cancelled"));
                }

                m_complete(std::move(result));
            }

            template <class Note>
            arch::SynthesisResult<void> prepare(const arch::MultiSingerContext &inputContext, const arch::Piece<Note> &piece) {
                auto resolved = synthesisContext(*catalog, inputContext);
                if (!resolved) {
                    return std::unexpected{std::move(resolved.error())};
                }

                context = std::move(*resolved);
                auto valid = validateMix(context, piece.mix, piece.mixSampleRate);
                if (!valid) {
                    return std::unexpected{std::move(valid.error())};
                }

                auto converted = synthesisScore(*context.sources.front().singer, piece);
                if (!converted) {
                    return std::unexpected{std::move(converted.error())};
                }

                score = std::move(*converted);
                duration = piece.pieceDuration;
                log.stdcTrace("Prepared synthesis score singer=%1 group=%2 sources=%3 notes=%4 words=%5 piece_seconds=%6", singer().info.id, context.group->info.id, context.sources.size(), piece.notes.size(), score.words.size(), duration);
                return {};
            }

            const SingerRecord &singer() const { return *context.sources.front().singer; }

            drogon::Task<bool> background() {
                auto ready = co_await tasks.resume(stop);
                if (!ready) {
                    if (ready.error().get_if<resource::TaskCancelledError>()) {
                        finish(fail<arch::SynthesisErrors>(arch::CancelledError("DiffSinger background task was cancelled")));
                    } else {
                        finish(fail<arch::SynthesisErrors>(arch::OperationError("Cannot execute DiffSinger background task", ready.error().asError().clone())));
                    }
                    co_return false;
                }

                co_return !cancelled();
            }

            template <class Executive, class Input>
            auto infer(SynthesisStage stage, std::shared_ptr<Input> input) -> drogon::Task<inference::ExecutionResult<typename decltype(std::declval<Executive &>().start(std::declval<const Input &>()))::value_type>> {
                if (stop.stop_requested()) co_return fail<inference::ExecutionErrors>(inference::CancelledError("DiffSinger synthesis request was cancelled"));
                const auto pool = catalog->environment()->inferences(synthesisCacheGroup(stage));
                if (!pool) co_return fail<inference::ExecutionErrors>(inference::ExecutionError("Synthesis cache group is unavailable for stage " + std::string(stageName(stage)), pool.error().clone()));
                auto handle = bindSynthesis(**pool, singer(), stage);
                if (!handle) co_return fail<inference::ExecutionErrors>(std::move(handle.error()));
                log.stdcTrace("Submitting %1 inference for singer %2 (piece=%3 seconds)", stageName(stage), singer().info.id, duration);
                auto result = co_await (*pool)->runCoro<Executive>(*handle, stop, [input](auto &executive) {
                    auto output = executive.start(*input);
                    if constexpr (std::is_same_v<Executive, Ac::AcousticExecutive>) {
                        // Preserve native tensor storage before releasing the lease.
                        if (output && *output) {
                            if ((*output)->mel) (*output)->mel = (*output)->mel->clone();
                            if ((*output)->f0) (*output)->f0 = (*output)->f0->clone();
                        }
                    }

                    return output;
                }, progress);
                if (!(co_await background())) co_return fail<inference::ExecutionErrors>(inference::CancelledError("DiffSinger continuation was cancelled"));
                co_return std::move(result);
            }

        private:
            arch::Completion<Output> m_complete;
            std::atomic_flag m_done;
        };

        template <class Output>
        arch::SynthesisResult<StageScore> preparedStage(const Request<Output> &request, SynthesisStage stage, const arch::Piece<arch::ParameterNote> &piece) {
            auto result = stageScore(request.score, request.context, stageName(stage), piece.mix, piece.mixSampleRate);
            if (!result) {
                return std::unexpected{std::move(result.error())};
            }

            auto valid = validateStageWords(request.singer(), stage, result->words);
            if (!valid) {
                return std::unexpected{std::move(valid.error())};
            }

            return result;
        }

        struct ParameterRequest final : Request<arch::ParameterOutput>, std::enable_shared_from_this<ParameterRequest> {
            using Request::Request;
            arch::ParameterInput input;
            std::map<std::string, Common::InputParameterInfo> curves;
            std::set<std::string> requested;
            std::set<std::string> recomputed;
            arch::ParameterOutput output;
            StageScore pitchScore;
            StageScore varianceScore;

            arch::SynthesisResult<void> prepareParameters() {
                auto valid = prepare(input.context, input.piece);
                if (!valid) {
                    return std::unexpected{std::move(valid.error())};
                }

                for (const auto &[id, parameter] : input.parameters) {
                    const auto capability = context.group->info.parameterPipeline.find(id);
                    if (capability == context.group->info.parameterPipeline.end()) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError("Group " + context.group->info.id + " does not support parameter " + id));
                    }

                    auto curve = parameterCurve(id, parameter.values, parameter.sampleRate, duration);
                    if (!curve) {
                        return std::unexpected{std::move(curve.error())};
                    }
                    if (parameter.retake) {
                        auto range = parameterRetake(*parameter.retake, parameter.sampleRate, duration, std::get_if<arch::IndirectParameter>(&capability->second));
                        if (!range) {
                            return fail<arch::SynthesisErrors>(arch::InvalidInputError("Invalid retake for parameter " + id, range.error().asError().clone()));
                        }

                        requested.insert(id);
                        curve->retake = *range;
                        if (parameter.retake->length) {
                            recomputed.insert(id);
                        } else {
                            auto values = parameter.values;
                            if (values.empty()) {
                                values.push_back(parameterDefinition(id)->defaultValue);
                            }
                            if (values.size() > 1) {
                                const auto count = sampleCount(duration, parameter.sampleRate);
                                if (!count) {
                                    return std::unexpected{count.error()};
                                }
                                values.resize(*count, parameterDefinition(id)->defaultValue);
                            }

                            output.parameters.emplace(id, arch::ParameterOutputParameter{std::move(values), parameter.sampleRate});
                        }
                    }
                    curves.emplace(id, std::move(*curve));
                }

                // Defaults are conditions. Generate the group's dependency closure
                // without asking the models to predict an unrequested parameter.
                for (const auto &[id, capability] : context.group->info.parameterPipeline) {
                    if (curves.contains(id)) {
                        continue;
                    }
                    auto curve = parameterCurve(id, {}, 100, duration);
                    if (!curve) {
                        return std::unexpected{std::move(curve.error())};
                    }
                    curves.emplace(id, std::move(*curve));
                }

                if (recomputed.contains("pitch")) {
                    auto converted = preparedStage(*this, SynthesisStage::Pitch, input.piece);
                    if (!converted) {
                        return std::unexpected{std::move(converted.error())};
                    }
                    pitchScore = std::move(*converted);
                }

                if (std::ranges::any_of(recomputed, [](const auto &id) { return id != "pitch"; })) {
                    auto converted = preparedStage(*this, SynthesisStage::Variance, input.piece);
                    if (!converted) {
                        return std::unexpected{std::move(converted.error())};
                    }
                    varianceScore = std::move(*converted);
                    const auto *schema = dynamic_cast<const Va::VarianceSchema *>(singer().stages.at("variance").spec->exports());
                    if (!schema) {
                        return fail<arch::SynthesisErrors>(arch::OperationError("Missing variance output schema for singer " + singer().info.id));
                    }

                    for (const auto &tag : schema->predictions) {
                        // Native variance runs every exported output, even if the
                        // singer imports only a subset. Lock all remaining outputs.
                        if (!parameterDefinition(tag.name())) {
                            return fail<arch::SynthesisErrors>(arch::OperationError("Variance schema exports an unsupported parameter: " + tag.name()));
                        }
                        if (!curves.contains(tag.name())) {
                            auto curve = parameterCurve(tag.name(), {}, 100, duration);
                            if (!curve) {
                                return std::unexpected{std::move(curve.error())};
                            }
                            curves.emplace(tag.name(), std::move(*curve));
                        }
                    }
                }

                return {};
            }

            arch::SynthesisResult<void> savePrediction(const std::string &id, std::vector<double> values, double interval) {
                const auto &condition = curves.at(id);
                auto normalized = parameterOutput(id, values, interval, duration);
                if (!normalized) {
                    return std::unexpected{std::move(normalized.error())};
                }

                // Apply the native grid's rounded retake boundary. Preserve input
                // control outside the range even if a model changes locked frames.
                const auto range = *condition.retake;
                const double start = std::round(range.start / interval);
                const double end = std::round(range.end / interval);
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (static_cast<double>(i) < start || static_cast<double>(i) >= end) {
                        values[i] = curveValue(condition, static_cast<double>(i) * interval, id == "pitch");
                    } else {
                        values[i] = engineValue(*parameterDefinition(id), normalized->values[i]);
                    }
                }

                normalized = parameterOutput(id, values, interval, duration);
                if (!normalized) {
                    return std::unexpected{std::move(normalized.error())};
                }

                output.parameters[id] = std::move(*normalized);
                Common::InputParameterInfo updated{condition.tag, std::move(values), interval, Common::InputParameterInfo::RetakeRange{0, 0}};
                if (updated.values.empty()) {
                    updated.values.push_back(engineValue(*parameterDefinition(id), parameterDefinition(id)->defaultValue));
                }

                curves[id] = std::move(updated);
                return {};
            }

            drogon::Task<> pitch() {
                if (cancelled()) {
                    co_return;
                }

                if (!recomputed.contains("pitch")) {
                    co_await variance();
                    co_return;
                }

                if (duration == 0) {
                    finish(fail<arch::SynthesisErrors>(arch::InvalidInputError("Pitch retake requires a nonempty piece time axis")));
                    co_return;
                }

                auto task = std::make_shared<Pi::PitchStartInput>();
                task->duration = duration;
                task->steps = context.options.steps.pitch;
                task->words = std::move(pitchScore.words);
                task->speakers = std::move(pitchScore.speakers);
                task->parameters.push_back(curves.at("pitch"));
                if (curves.contains("expressiveness") && context.group->info.parameterPipeline.contains("expressiveness")) {
                    task->parameters.push_back(curves.at("expressiveness"));
                }

                auto result = co_await infer<Pi::PitchExecutive>(SynthesisStage::Pitch, task);
                if (!result) {
                    finish(std::unexpected{synthesisExecutionError(result.error(), "pitch", singer())});
                    co_return;
                }
                if (!*result) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Pitch inference returned a null result for singer " + singer().info.id)));
                    co_return;
                }

                auto saved = savePrediction("pitch", std::move((*result)->pitch), (*result)->interval);
                if (!saved) {
                    finish(std::unexpected{std::move(saved.error())});
                    co_return;
                }

                progress.advance(std::ranges::any_of(recomputed, [](const auto &id) { return id != "pitch"; }) ? 0.5 : 1);
                co_await variance();
            }

            drogon::Task<> variance() {
                if (cancelled()) {
                    co_return;
                }
                if (std::ranges::none_of(recomputed, [](const auto &id) { return id != "pitch"; })) {
                    finish(std::move(output));
                    co_return;
                }

                const auto &stage = singer().stages.at("variance");
                const auto *configuration = dynamic_cast<const Va::VarianceConfiguration *>(stage.spec->configuration());
                const auto *schema = dynamic_cast<const Va::VarianceSchema *>(stage.spec->exports());
                if (!configuration || !schema) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Missing typed variance configuration for singer " + singer().info.id)));
                    co_return;
                }

                const auto tone = curves.find("tone_shift");
                auto pitchCurve = effectivePitch(curves.at("pitch"), tone == curves.end() ? nullptr : &tone->second, duration, configuration->frameWidth);
                if (!pitchCurve) {
                    finish(std::unexpected{std::move(pitchCurve.error())});
                    co_return;
                }

                auto task = std::make_shared<Va::VarianceStartInput>();
                task->duration = duration;
                task->steps = context.options.steps.variance;
                task->words = std::move(varianceScore.words);
                task->speakers = std::move(varianceScore.speakers);
                task->parameters.push_back(std::move(*pitchCurve));
                for (const auto &tag : schema->predictions) {
                    task->parameters.push_back(curves.at(tag.name()));
                }

                auto result = co_await infer<Va::VarianceExecutive>(SynthesisStage::Variance, task);
                if (!result) {
                    finish(std::unexpected{synthesisExecutionError(result.error(), "variance", singer())});
                    co_return;
                }
                if (!*result || (*result)->predictions.size() != schema->predictions.size()) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Variance inference returned an incomplete output set for singer " + singer().info.id)));
                    co_return;
                }

                for (std::size_t i = 0; i < schema->predictions.size(); ++i) {
                    auto &prediction = (*result)->predictions[i];
                    if (prediction.tag != schema->predictions[i]) {
                        finish(fail<arch::SynthesisErrors>(arch::OperationError("Variance output tag/order does not match its schema")));
                        co_return;
                    }
                    if (recomputed.contains(prediction.tag.name())) {
                        auto saved = savePrediction(prediction.tag.name(), std::move(prediction.values), prediction.interval);
                        if (!saved) {
                            finish(std::unexpected{std::move(saved.error())});
                            co_return;
                        }
                    } else {
                        auto valid = parameterOutput(prediction.tag.name(), prediction.values, prediction.interval, duration);
                        if (!valid) {
                            finish(std::unexpected{std::move(valid.error())});
                            co_return;
                        }
                    }
                }

                for (const auto &id : requested) {
                    if (!output.parameters.contains(id)) {
                        finish(fail<arch::SynthesisErrors>(arch::OperationError("Variance inference did not return requested parameter " + id)));
                        co_return;
                    }
                }

                progress.advance(1);
                finish(std::move(output));
            }
        };

        struct AudioRequest final : Request<arch::AudioOutput>, std::enable_shared_from_this<AudioRequest> {
            using Request::Request;
            arch::AudioInput input;
            std::map<std::string, Common::InputParameterInfo> curves;
            StageScore acousticScore;

            arch::SynthesisResult<void> prepareAudio() {
                auto valid = prepare(input.context, input.piece);
                if (!valid) {
                    return std::unexpected{std::move(valid.error())};
                }
                if (!input.media.acceptableSchemes.empty() && std::ranges::none_of(input.media.acceptableSchemes, [](const auto &value) { return stdc::str::equals_insensitive(value, "data"); })) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError("acceptableSchemes does not accept the synthesized audio scheme data"));
                }
                if (!input.media.acceptableFormats.empty() && std::ranges::none_of(input.media.acceptableFormats, [](const auto &value) { return stdc::str::equals_insensitive(value, "audio/wav"); })) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError("acceptableFormats does not accept the synthesized audio format audio/wav"));
                }

                valid = validateMelConfiguration(singer());
                if (!valid) {
                    return std::unexpected{std::move(valid.error())};
                }

                for (const auto &id : context.group->info.audioDependencies) {
                    if (!input.parameters.contains(id)) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError("Audio synthesis requires parameter " + id + " for group " + context.group->info.id));
                    }
                }

                for (const auto &[id, parameter] : input.parameters) {
                    if (!context.group->info.parameterPipeline.contains(id)) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError("Group " + context.group->info.id + " does not support parameter " + id));
                    }
                    const auto values = parameter.values ? std::span<const double>(*parameter.values) : std::span<const double>{};
                    auto curve = parameterCurve(id, values, parameter.sampleRate, duration);
                    if (!curve) {
                        return std::unexpected{std::move(curve.error())};
                    }
                    curves.emplace(id, std::move(*curve));
                }

                auto converted = preparedStage(*this, SynthesisStage::Acoustic, input.piece);
                if (!converted) {
                    return std::unexpected{std::move(converted.error())};
                }

                acousticScore = std::move(*converted);
                return {};
            }

            void encode(std::span<const std::uint8_t> pcm) {
                if (cancelled()) {
                    return;
                }

                const auto *configuration = dynamic_cast<const Vo::VocoderConfiguration *>(singer().stages.at("vocoder").spec->configuration());
                auto url = media::waveDataUrl(pcm, configuration->sampleRate, duration);
                if (!url) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Cannot encode synthesized WAV for singer " + singer().info.id, url.error().clone())));
                } else {
                    finish(arch::AudioOutput{std::move(*url)});
                }
            }

            drogon::Task<> acoustic() {
                if (cancelled()) {
                    co_return;
                }
                if (duration == 0 || score.silent) {
                    encode({});
                    co_return;
                }

                const auto *configuration = dynamic_cast<const Ac::AcousticConfiguration *>(singer().stages.at("acoustic").spec->configuration());
                const auto *schema = dynamic_cast<const Ac::AcousticSchema *>(singer().stages.at("acoustic").spec->exports());
                const auto tone = curves.find("tone_shift");
                auto pitch = effectivePitch(curves.at("pitch"), tone == curves.end() ? nullptr : &tone->second, duration, static_cast<double>(configuration->hopSize) / configuration->sampleRate);
                if (!pitch) {
                    finish(std::unexpected{std::move(pitch.error())});
                    co_return;
                }

                auto task = std::make_shared<Ac::AcousticStartInput>();
                task->duration = duration;
                task->steps = context.options.steps.acoustic;
                task->depth = context.options.depth;
                task->words = std::move(acousticScore.words);
                task->speakers = std::move(acousticScore.speakers);
                task->parameters.push_back(std::move(*pitch));
                for (const auto &[id, curve] : curves) {
                    if (id == "pitch" || id == "tone_shift" || id == "expressiveness") {
                        continue;
                    }
                    if (configuration->parameters.contains(curve.tag) && (schema->varianceControls.contains(curve.tag) || schema->transitionControls.contains(curve.tag))) {
                        task->parameters.push_back(curve);
                    }
                }

                auto result = co_await infer<Ac::AcousticExecutive>(SynthesisStage::Acoustic, task);
                if (!result) {
                    finish(std::unexpected{synthesisExecutionError(result.error(), "acoustic", singer())});
                } else if (!*result) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Acoustic inference returned a null result for singer " + singer().info.id)));
                } else {
                    co_await vocoder(std::move(*result));
                }
            }

            drogon::Task<> vocoder(std::unique_ptr<Ac::AcousticResult> result) {
                if (cancelled()) {
                    co_return;
                }

                const auto *configuration = dynamic_cast<const Ac::AcousticConfiguration *>(singer().stages.at("acoustic").spec->configuration());
                const auto *vocoderConfiguration = dynamic_cast<const Vo::VocoderConfiguration *>(singer().stages.at("vocoder").spec->configuration());
                const auto frames = acousticFrames(*result, configuration->melChannels, singer().info.id);
                if (!frames) {
                    finish(std::unexpected{frames.error()});
                    co_return;
                }

                progress.advance(0.9);
                auto task = std::make_shared<Vo::VocoderStartInput>();
                task->mel = std::move(result->mel);
                if (vocoderConfiguration->pitchControllable && curves.contains("tone_shift")) {
                    auto f0 = vocoderF0(curves.at("pitch"), *frames, static_cast<double>(configuration->hopSize) / configuration->sampleRate);
                    if (!f0) {
                        finish(std::unexpected{std::move(f0.error())});
                        co_return;
                    }

                    auto tensor = inference::nativeCall("Tensor::createFromView(original pitch f0)", [&] { return ds::Tensor::createFromView<float>({1, static_cast<std::int64_t>(*frames)}, stdc::array_view<float>(f0->data(), f0->size())); });
                    if (!tensor) {
                        finish(fail<arch::SynthesisErrors>(arch::OperationError("Cannot construct original pitch vocoder tensor for singer " + singer().info.id, tensor.error().clone())));
                        co_return;
                    }

                    task->f0 = std::move(*tensor);
                } else {
                    task->f0 = std::move(result->f0);
                }
                if (!task->mel || !task->f0) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Acoustic tensor clone returned no storage for singer " + singer().info.id)));
                    co_return;
                }

                auto output = co_await infer<Vo::VocoderExecutive>(SynthesisStage::Vocoder, task);
                if (!output) {
                    finish(std::unexpected{synthesisExecutionError(output.error(), "vocoder", singer())});
                } else if (!*output) {
                    finish(fail<arch::SynthesisErrors>(arch::OperationError("Vocoder inference returned a null result for singer " + singer().info.id)));
                } else {
                    progress.advance(1);
                    encode((*output)->audioData);
                }
            }
        };
    }

    void synthesizeDuration(std::shared_ptr<const SingerCatalog> catalog, resource::TaskExecutor &tasks, arch::DurationInput input, std::stop_token stop, arch::Completion<arch::DurationOutput> complete) {
        const auto request = std::make_shared<Request<arch::DurationOutput>>(std::move(catalog), tasks, stop, std::move(complete));
        request->progress = input.progress;
        if (request->cancelled()) {
            return;
        }

        drogon::async_run([request, input = std::move(input)]() -> drogon::Task<> {
            if (!(co_await request->background())) co_return;
            if (request->cancelled()) {
                co_return;
            }

            auto valid = request->prepare(input.context, input.piece);
            if (!valid) {
                request->finish(std::unexpected{std::move(valid.error())});
                co_return;
            }

            if (request->context.group->info.durationMode != arch::DurationMode::Full) {
                request->finish(fail<arch::SynthesisErrors>(arch::InvalidInputError("Group " + request->context.group->info.id + " does not provide duration prediction")));
                co_return;
            }

            if (input.piece.notes.empty()) {
                request->finish(arch::DurationOutput{});
                co_return;
            }

            auto score = stageScore(request->score, request->context, "duration", input.piece.mix, input.piece.mixSampleRate);
            if (!score) {
                request->finish(std::unexpected{std::move(score.error())});
                co_return;
            }

            valid = validateStageWords(request->singer(), SynthesisStage::Duration, score->words);
            if (!valid) {
                request->finish(std::unexpected{std::move(valid.error())});
                co_return;
            }

            auto task = std::make_shared<Du::DurationStartInput>();
            task->duration = input.piece.pieceDuration;
            task->words = std::move(score->words);
            auto result = co_await request->infer<Du::DurationExecutive>(SynthesisStage::Duration, task);
            if (!result) {
                request->finish(std::unexpected{synthesisExecutionError(result.error(), "duration", request->singer())});
            } else if (!*result) {
                request->finish(fail<arch::SynthesisErrors>(arch::OperationError("Duration inference returned a null result for singer " + request->singer().info.id)));
            } else {
                request->progress.advance(1);
                request->finish(durationOutput(request->score, (*result)->durations));
            }
        });
    }

    void synthesizeParameters(std::shared_ptr<const SingerCatalog> catalog, resource::TaskExecutor &tasks, arch::ParameterInput input, std::stop_token stop, arch::Completion<arch::ParameterOutput> complete) {
        const auto request = std::make_shared<ParameterRequest>(std::move(catalog), tasks, stop, std::move(complete));
        request->progress = input.progress;
        if (request->cancelled()) {
            return;
        }

        request->input = std::move(input);
        drogon::async_run([request]() -> drogon::Task<> {
            if (!(co_await request->background())) co_return;
            auto valid = request->prepareParameters();
            if (!valid) request->finish(std::unexpected{std::move(valid.error())});
            else co_await request->pitch();
        });
    }

    void synthesizeAudio(std::shared_ptr<const SingerCatalog> catalog, resource::TaskExecutor &tasks, arch::AudioInput input, std::stop_token stop, arch::Completion<arch::AudioOutput> complete) {
        const auto request = std::make_shared<AudioRequest>(std::move(catalog), tasks, stop, std::move(complete));
        request->progress = input.progress;
        if (request->cancelled()) {
            return;
        }

        request->input = std::move(input);
        drogon::async_run([request]() -> drogon::Task<> {
            if (!(co_await request->background())) co_return;
            auto valid = request->prepareAudio();
            if (!valid) request->finish(std::unexpected{std::move(valid.error())});
            else co_await request->acoustic();
        });
    }
}
