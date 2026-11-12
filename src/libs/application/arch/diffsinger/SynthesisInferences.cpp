#include "SynthesisInferences.h"

#include <exception>
#include <functional>
#include <mutex>

#include <dsinfer/Api/Singers/DiffSinger/1/DiffSingerApiL1.h>

#include <dsspinference/ResourceIdentity.h>

namespace dssp::arch_diffsinger {
    namespace {
        namespace Ds = ds::Api::DiffSinger::L1;
        namespace Du = ds::Api::Duration::L1;
        namespace Pi = ds::Api::Pitch::L1;
        namespace Va = ds::Api::Variance::L1;
        namespace Ac = ds::Api::Acoustic::L1;
        namespace Vo = ds::Api::Vocoder::L1;

        struct PipelineFactory {
            std::mutex mutex;
            // Metadata factory survives eviction. Child models are owned by the pool.
            std::unique_ptr<srt::SingerPipelineExecutive> pipeline;
        };

        template <class Executive, class Options, class InitArgs, class Create>
        std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> createStage(Ds::DiffSingerPipelineExecutive &pipeline, Create create) {
            auto created = inference::nativeCall("DiffSinger pipeline create stage", [&] { return std::invoke(create, pipeline, Options{}); });
            if (!created) {
                return std::unexpected{std::move(created.error())};
            }

            // The native supervision contract permits early deletion and detaches
            // the child. The pipeline remains alive in the resource definition.
            std::unique_ptr<Executive> executive(*created);
            if (!executive) {
                return std::unexpected{inference::ExecutionError("DiffSinger pipeline returned a null stage executive")};
            }

            auto initialized = inference::nativeCall("DiffSinger stage initialize", [&] { return executive->initialize(InitArgs{}); });
            if (!initialized) {
                return std::unexpected{std::move(initialized.error())};
            }

            return std::unique_ptr<srt::InferenceExecutive>(std::move(executive));
        }

        template <class Configuration>
        arch::SynthesisResult<void> validateWords(const StageRecord &stage, const std::vector<Common::InputWordInfo> &words, std::string_view name, const localization::Context &locale) {
            const auto *configuration = dynamic_cast<const Configuration *>(stage.spec->configuration());
            if (!configuration) {
                return fail<arch::SynthesisErrors>(arch::OperationError("Missing typed configuration for stage " + std::string(name)));
            }

            for (std::size_t i = 0; i < words.size(); ++i) {
                for (std::size_t j = 0; j < words[i].phones.size(); ++j) {
                    const auto &phone = words[i].phones[j];
                    const auto qualified = phone.language.empty() || phone.token == "SP" || phone.token == "AP" ? phone.token : phone.language + '/' + phone.token;
                    if (!configuration->phonemes.contains(qualified) && !configuration->phonemes.contains(phone.token)) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.stage_phoneme_token_unsupported", "Stage {0} word {1} phoneme {2} does not accept token {3}", name, std::to_string(i), std::to_string(j), qualified)));
                    }
                    if (configuration->useLanguageId && !configuration->languages.contains(phone.language)) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.stage_phoneme_language_unsupported", "Stage {0} does not accept phoneme language '{1}' for token {2}", name, phone.language, phone.token)));
                    }
                }
            }

            return {};
        }
    }

    std::string_view stageName(SynthesisStage stage) {
        switch (stage) {
            case SynthesisStage::Duration: return "duration";
            case SynthesisStage::Pitch: return "pitch";
            case SynthesisStage::Variance: return "variance";
            case SynthesisStage::Acoustic: return "acoustic";
            case SynthesisStage::Vocoder: return "vocoder";
        }
        std::unreachable();
    }

    std::expected<std::string, inference::ExecutionError> synthesisInferenceKey(const SingerRecord &singer, SynthesisStage stage) {
        try {
            const auto found = singer.stages.find(std::string(stageName(stage)));
            if (found == singer.stages.end() || !found->second.spec) {
                return std::unexpected{inference::ExecutionError("Missing synthesis inference for stage " + std::string(stageName(stage)))};
            }

            const auto &record = found->second;
            const auto imported = singer.spec->findImport("singer/" + std::string(stageName(stage)));
            if (!imported) {
                return std::unexpected{inference::ExecutionError("Missing synthesis stage import")};
            }

            auto effective = imported->manifestOptions().toObject();
            // Speaker mapping is applied to each source's input, not to the model.
            effective.erase("speakerMapping");
            auto key = inference::fingerprint(srt::JsonValue(srt::JsonObject{{"stage", std::string(stageName(stage))}, {"resource", inference::resourceId(*record.spec)}, {"options", std::move(effective)}}).toJson());
            if (!key) {
                return std::unexpected{inference::ExecutionError("Cannot identify synthesis inference", key.error().clone())};
            }

            return *key;
        } catch (const std::exception &error) {
            return std::unexpected{inference::ExecutionError("Cannot read the synthesis inference identity", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"dsinfer/synthrt", "identify synthesis stage", "exception"}))};
        }
    }

    std::string synthesisCacheGroup(SynthesisStage stage) {
        return "diffsinger-" + std::string(stageName(stage));
    }

    std::expected<inference::InferenceHandle, inference::ExecutionError> bindSynthesis(inference::InferencePool &pool, const SingerRecord &singer, SynthesisStage stage) {
        auto key = synthesisInferenceKey(singer, stage);
        if (!key) {
            return std::unexpected{std::move(key.error())};
        }

        try {
            const auto &record = singer.stages.at(std::string(stageName(stage)));
            inference::InferenceDefinition definition;
            definition.key = *key;
            definition.label = inference::resourceReference(*record.spec);
            definition.packages = {singer.package, record.spec->package()};

            auto factory = std::make_shared<PipelineFactory>();
            definition.create = [factory, spec = singer.spec, stage]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                const std::lock_guard lock(factory->mutex);
                if (!factory->pipeline) {
                    auto *extension = srt::ContribSpecExtension::findFromSpec<Ds::DiffSingerPipelineExecutive>(*spec);
                    auto *typed = dynamic_cast<srt::SingerPipelineExtension *>(extension);
                    if (!typed) {
                        return std::unexpected{inference::ExecutionError("DiffSinger pipeline extension is unavailable")};
                    }

                    auto created = inference::nativeCall("SingerPipelineExtension::createPipeline", [&] { return typed->createPipeline(Ds::DiffSingerPipelineRuntimeOptions{}); });
                    if (!created) {
                        return std::unexpected{std::move(created.error())};
                    }
                    factory->pipeline = std::move(*created);
                }

                auto *pipeline = dynamic_cast<Ds::DiffSingerPipelineExecutive *>(factory->pipeline.get());
                if (!pipeline) {
                    return std::unexpected{inference::ExecutionError("Singer provider returned an incompatible pipeline")};
                }

                switch (stage) {
                    case SynthesisStage::Duration: return createStage<Du::DurationExecutive, Du::DurationRuntimeOptions, Du::DurationInitArgs>(*pipeline, &Ds::DiffSingerPipelineExecutive::createDuration);
                    case SynthesisStage::Pitch: return createStage<Pi::PitchExecutive, Pi::PitchRuntimeOptions, Pi::PitchInitArgs>(*pipeline, &Ds::DiffSingerPipelineExecutive::createPitch);
                    case SynthesisStage::Variance: return createStage<Va::VarianceExecutive, Va::VarianceRuntimeOptions, Va::VarianceInitArgs>(*pipeline, &Ds::DiffSingerPipelineExecutive::createVariance);
                    case SynthesisStage::Acoustic: return createStage<Ac::AcousticExecutive, Ac::AcousticRuntimeOptions, Ac::AcousticInitArgs>(*pipeline, &Ds::DiffSingerPipelineExecutive::createAcoustic);
                    case SynthesisStage::Vocoder: return createStage<Vo::VocoderExecutive, Vo::VocoderRuntimeOptions, Vo::VocoderInitArgs>(*pipeline, &Ds::DiffSingerPipelineExecutive::createVocoder);
                }
                std::unreachable();
            };

            return pool.bind(std::move(definition));
        } catch (const std::exception &error) {
            return std::unexpected{inference::ExecutionError("Cannot read the synthesis inference contract", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"dsinfer/synthrt", "bind synthesis stage", "exception"}))};
        }
    }

    arch::SynthesisErrors synthesisExecutionError(const inference::ExecutionErrors &error, std::string_view stage, const SingerRecord &singer) {
        const auto context = "DiffSinger " + std::string(stage) + " for singer " + singer.info.id;
        if (error.get_if<inference::CancelledError>()) {
            return arch::SynthesisErrors{arch::CancelledError(context + " was cancelled", error.asError().clone())};
        }

        return arch::SynthesisErrors{arch::OperationError(context + " failed", error.asError().clone())};
    }

    arch::SynthesisResult<void> validateStageWords(const SingerRecord &singer, SynthesisStage stage, const std::vector<Common::InputWordInfo> &words, const localization::Context &locale) {
        const auto name = stageName(stage);
        const auto found = singer.stages.find(std::string(name));
        if (found == singer.stages.end() || !found->second.spec) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.singer_operation_unavailable", "Singer {0} does not provide {1}", singer.info.id, name)));
        }

        switch (stage) {
            case SynthesisStage::Duration: return validateWords<Du::DurationConfiguration>(found->second, words, name, locale);
            case SynthesisStage::Pitch: return validateWords<Pi::PitchConfiguration>(found->second, words, name, locale);
            case SynthesisStage::Variance: return validateWords<Va::VarianceConfiguration>(found->second, words, name, locale);
            case SynthesisStage::Acoustic: return validateWords<Ac::AcousticConfiguration>(found->second, words, name, locale);
            case SynthesisStage::Vocoder: return {};
        }
        std::unreachable();
    }

    arch::SynthesisResult<void> validateMelConfiguration(const SingerRecord &singer) {
        const auto *acoustic = dynamic_cast<const Ac::AcousticConfiguration *>(singer.stages.at("acoustic").spec->configuration());
        const auto *vocoder = dynamic_cast<const Vo::VocoderConfiguration *>(singer.stages.at("vocoder").spec->configuration());
        if (!acoustic || !vocoder || acoustic->sampleRate <= 0 || acoustic->hopSize <= 0 || acoustic->melChannels <= 0) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Singer " + singer.info.id + " has an invalid acoustic/vocoder configuration"));
        }
        if (acoustic->sampleRate != vocoder->sampleRate || acoustic->hopSize != vocoder->hopSize || acoustic->winSize != vocoder->winSize || acoustic->fftSize != vocoder->fftSize || acoustic->melChannels != vocoder->melChannels || acoustic->melMinFreq != vocoder->melMinFreq || acoustic->melMaxFreq != vocoder->melMaxFreq || acoustic->melBase != vocoder->melBase || acoustic->melScale != vocoder->melScale) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Singer " + singer.info.id + " imports incompatible acoustic and vocoder mel configurations"));
        }

        return {};
    }
}
