#include "SingerCatalog.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <dsinfer/Api/Singers/DiffSinger/1/DiffSingerApiL1.h>
#include <stdcorelib/support/logging.h>
#include <synthrt/Core/ContribImportBinding.h>
#include <wolf/Linguist/LinguistContrib.h>
#include <wolf/Api/Linguists/Linguist/1/LinguistApiL1.h>

#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/SingerMetadata.h>
#include <dsspinference/Display.h>
#include <dsspinference/LanguageMetadata.h>
#include <dsspinference/MediaPath.h>
#include <dsspinference/ResourceIdentity.h>

namespace dssp::arch_diffsinger {
    namespace {
        stdc::LogCategory log("dssp.arch_diffsinger.SingerCatalog");
        namespace Du = ds::Api::Duration::L1;
        namespace Pi = ds::Api::Pitch::L1;
        namespace Va = ds::Api::Variance::L1;
        namespace Ac = ds::Api::Acoustic::L1;
        namespace Vo = ds::Api::Vocoder::L1;
        namespace Ds = ds::Api::DiffSinger::L1;
        using Depth = inference::LanguageDepth;

        template <class Schema, class Configuration, class Options>
        std::expected<StageRecord, inference::ResourceError> readStage(const srt::SingerSpec &singer, std::string_view role, bool required) {
            const auto imported = singer.findImport(role);
            if (!imported) {
                if (required) {
                    return std::unexpected{inference::ResourceError("Missing required stage " + std::string(role))};
                }
                return StageRecord{};
            }
            if (!imported->binding() || imported->binding()->state() != srt::ContribImportBinding::Active) {
                return std::unexpected{inference::ResourceError("Unresolved stage " + std::string(role))};
            }

            const auto &target = imported->binding()->target();
            const auto *schema = dynamic_cast<const Schema *>(target.exports());
            const auto *configuration = dynamic_cast<const Configuration *>(target.configuration());
            const auto *options = dynamic_cast<const Options *>(imported->options());
            if (!schema || !configuration || !options || target.interface() != schema->interface() || target.variant() != schema->variant() || target.level() != schema->level()) {
                return std::unexpected{inference::ResourceError("Unsupported typed stage contract for " + std::string(role))};
            }

            StageRecord result;
            result.spec = &target;
            result.options = options;
            if constexpr (requires { configuration->useSpeakerEmbedding; schema->speakers; options->speakerMapping; }) {
                result.usesSpeakers = configuration->useSpeakerEmbedding;
                if (result.usesSpeakers) {
                    result.speakers = schema->speakers;
                    result.speakerMapping = options->speakerMapping;
                    for (const auto &[speaker, embedding] : configuration->speakers) {
                        if (embedding.size() == static_cast<std::size_t>(configuration->hiddenSize) && !embedding.empty() && std::ranges::all_of(embedding, [](float value) { return std::isfinite(value); })) {
                            result.embeddedSpeakers.insert(speaker);
                        } else {
                            log.stdcWarning("Singer %1 stage %2 excludes invalid embedding for speaker %3 (%4 values, expected %5 finite values)", inference::resourceReference(singer), role, speaker, embedding.size(), configuration->hiddenSize);
                        }
                    }
                }
            }

            if (log.isLevelEnabled(stdc::Logger::Trace)) {
                log.stdcTrace("Singer %1 stage %2 resolves to %3 (speaker embeddings=%4, valid embeddings=%5)", inference::resourceReference(singer), role, inference::resourceReference(target), result.usesSpeakers, result.embeddedSpeakers.size());
            }

            return result;
        }

        const char *depthName(Depth depth) {
            switch (depth) {
                case Depth::Pronunciation: return "pronunciation";
                case Depth::Phonemes: return "phonemes";
                case Depth::Onsets: return "onsets";
            }
            std::unreachable();
        }

        template <class Configuration>
        bool acceptsPhoneme(const StageRecord &stage, const std::string &token, const std::string &qualified) {
            if (!stage.spec) {
                return true;
            }

            const auto *configuration = dynamic_cast<const Configuration *>(stage.spec->configuration());
            return configuration->phonemes.contains(qualified) || configuration->phonemes.contains(token);
        }

        std::expected<std::pair<SingerRecord, GroupRecord>, inference::ResourceError> readSinger(const srt::SingerSpec &singer) {
            if (!srt::ContribSpecExtension::findFromSpec<Ds::DiffSingerPipelineExecutive>(singer)) {
                return std::unexpected{inference::ResourceError("The DiffSinger pipeline provider is unavailable")};
            }

            const auto *configuration = dynamic_cast<const Ds::DiffSingerConfiguration *>(singer.configuration());
            if (!configuration) {
                return std::unexpected{inference::ResourceError("The DiffSinger configuration is unavailable")};
            }

            auto duration = readStage<Du::DurationSchema, Du::DurationConfiguration, Du::DurationImportOptions>(singer, "singer/duration", false);
            auto pitch = readStage<Pi::PitchSchema, Pi::PitchConfiguration, Pi::PitchImportOptions>(singer, "singer/pitch", false);
            auto variance = readStage<Va::VarianceSchema, Va::VarianceConfiguration, Va::VarianceImportOptions>(singer, "singer/variance", false);
            auto acoustic = readStage<Ac::AcousticSchema, Ac::AcousticConfiguration, Ac::AcousticImportOptions>(singer, "singer/acoustic", true);
            auto vocoder = readStage<Vo::VocoderSchema, Vo::VocoderConfiguration, Vo::VocoderImportOptions>(singer, "singer/vocoder", true);
            for (const auto *stage : {&duration, &pitch, &variance, &acoustic, &vocoder}) {
                if (!*stage) {
                    return std::unexpected{stage->error()};
                }
            }

            SingerRecord record;
            record.package = singer.package();
            record.spec = &singer;
            record.reservedPhonemes = singer.reservedPhonemes();
            record.info.id = inference::resourceReference(singer);
            record.info.arch = Id;
            record.name = singer.name();
            const auto readMediaPath = [&singer](std::string_view field, const srt::DisplayText &resolved) -> std::expected<srt::DisplayText, inference::ResourceError> {
                const auto &declaration = singer.manifestDeclaration();
                const auto found = declaration.find(std::string(field));
                return found == declaration.end() ? std::expected<srt::DisplayText, inference::ResourceError>{resolved} : inference::preserveEmptyMediaPaths(resolved, found->second);
            };

            auto avatar = readMediaPath("avatar", singer.avatar());
            auto background = readMediaPath("background", singer.background());
            auto demoAudio = readMediaPath("demoAudio", singer.demoAudio());
            for (const auto *path : {&avatar, &background, &demoAudio}) {
                if (!*path) {
                    return std::unexpected{path->error()};
                }
            }

            record.avatar = std::move(*avatar);
            record.background = std::move(*background);
            record.demoAudio = std::move(*demoAudio);
            record.stages = {{"duration", *duration}, {"pitch", *pitch}, {"variance", *variance}, {"acoustic", *acoustic}, {"vocoder", *vocoder}};
            record.speakers = commonSpeakers(record.stages);
            const bool needsSpeakers = std::ranges::any_of(record.stages, [](const auto &entry) { return entry.second.usesSpeakers; });
            if (needsSpeakers && record.speakers.empty()) {
                return std::unexpected{inference::ResourceError("No logical speaker is valid across the singer's effective stages")};
            }

            record.permitsMixing = singerMetadata(*configuration).permitsMixing;

            GroupRecord groupRecord;
            auto &group = groupRecord.info;
            group.arch = Id;
            group.durationMode = duration->spec ? arch::DurationMode::Full : arch::DurationMode::Skip;
            group.archSpecificInfo = Json::Value(Json::objectValue);
            bool completeLanguagePipeline = false;
            std::map<std::string, Depth> languageDepths;
            completeLanguagePipeline = !singer.languages().empty();
            for (const auto &[handle, role] : singer.languages()) {
                auto route = inference::readLanguageRoute(singer, handle);
                if (!route) {
                    return std::unexpected{std::move(route.error())};
                }

                const auto *exports = dynamic_cast<const wolf::Api::Linguist::L1::LinguistExports *>(route->spec->exports());
                if (!exports) {
                    return std::unexpected{inference::ResourceError("Missing linguist exports for " + handle)};
                }

                // Preserve Wolf's complete-inventory zero-coverage verdict.
                // Unknown and open inventories do not justify dropping a route.
                const auto accepted = [&](const std::string &token) {
                    // DiffSinger resolves language-qualified tokens before falling back to bare tokens in each stage.
                    const auto qualified = handle.empty() || token == "SP" || token == "AP" ? token : handle + '/' + token;
                    return acceptsPhoneme<Du::DurationConfiguration>(*duration, token, qualified)
                        && acceptsPhoneme<Pi::PitchConfiguration>(*pitch, token, qualified)
                        && acceptsPhoneme<Va::VarianceConfiguration>(*variance, token, qualified)
                        && acceptsPhoneme<Ac::AcousticConfiguration>(*acoustic, token, qualified);
                };
                if (!exports->openSet && !exports->phonemes.empty() && std::ranges::none_of(exports->phonemes, accepted)) {
                    log.stdcWarning("Singer %1 language %2 has no phonemes accepted by its synthesis stages", record.info.id, handle);
                    completeLanguagePipeline = false;
                    continue;
                }

                log.stdcTrace("Singer %1 language %2 supports depth %3", record.info.id, handle, depthName(route->depth));
                completeLanguagePipeline = completeLanguagePipeline && route->depth == Depth::Onsets;
                auto metadata = inference::languageMetadata(*route->spec);
                const auto phonemeMode = route->depth == Depth::Onsets ? arch::PhonemeMode::Full : route->depth == Depth::Phonemes ? arch::PhonemeMode::TokenOnly : arch::PhonemeMode::Skip;
                group.languages.emplace(handle, arch::GroupLanguageInfo{handle, std::move(metadata.defaultLyric), arch::PronunciationMode::Full, phonemeMode});
                groupRecord.languageNames.emplace(handle, std::move(metadata.name));
                languageDepths.emplace(handle, route->depth);
                record.languages.emplace(handle, std::move(*route));
            }

            if (group.languages.empty()) {
                log.stdcDebug("Singer %1 has no usable language declarations; pronunciation and phoneme stages will be skipped", record.info.id);
                completeLanguagePipeline = false;
            } else if (!singer.defaultLanguage().empty()) {
                if (!group.languages.contains(singer.defaultLanguage())) {
                    return std::unexpected{inference::ResourceError("The declared default language is unavailable: " + singer.defaultLanguage())};
                }

                record.info.defaultLanguage = singer.defaultLanguage();
            }

            const auto *acousticSchema = dynamic_cast<const Ac::AcousticSchema *>(acoustic->spec->exports());
            const auto *acousticConfiguration = dynamic_cast<const Ac::AcousticConfiguration *>(acoustic->spec->configuration());
            const auto *vocoderConfiguration = dynamic_cast<const Vo::VocoderConfiguration *>(vocoder->spec->configuration());
            const auto supported = [&](std::string_view parameter) {
                const ds::ParamTag tag{std::string(parameter)};
                return acousticConfiguration->parameters.contains(tag) && (acousticSchema->varianceControls.contains(tag) || acousticSchema->transitionControls.contains(tag));
            };

            bool expressiveness = false;
            if (pitch->spec) {
                const auto *schema = dynamic_cast<const Pi::PitchSchema *>(pitch->spec->exports());
                const auto *config = dynamic_cast<const Pi::PitchConfiguration *>(pitch->spec->configuration());
                expressiveness = schema->allowExpressiveness && config->useExpressiveness;
                group.parameterPipeline.emplace("pitch", arch::IndirectParameter{expressiveness ? std::vector<std::string>{"expressiveness"} : std::vector<std::string>{}, arch::ParameterRetakeMode::Range});
            } else {
                group.parameterPipeline.emplace("pitch", arch::DirectParameter{});
            }
            group.audioDependencies.push_back("pitch");
            if (expressiveness) {
                group.parameterPipeline.emplace("expressiveness", arch::DirectParameter{});
            }

            std::map<std::string, srt::JsonValue> effectiveOptions;
            for (const auto role : {"singer/duration", "singer/pitch", "singer/acoustic", "singer/vocoder"}) {
                effectiveOptions.emplace(role, srt::JsonObject{});
            }
            if (variance->spec) {
                const auto *schema = dynamic_cast<const Va::VarianceSchema *>(variance->spec->exports());
                const auto *options = dynamic_cast<const Va::VarianceImportOptions *>(variance->options);
                if (options->predictions.empty()) {
                    return std::unexpected{inference::ResourceError("The variance import must select a nonempty prediction subset")};
                }

                srt::JsonArray predictions;
                for (const auto &prediction : schema->predictions) {
                    if (options->predictions.contains(prediction)) {
                        predictions.push_back(prediction.name());
                        if (supported(prediction.name())) {
                            record.variancePredictions.push_back(prediction.name());
                        }
                    }
                }
                effectiveOptions.emplace("singer/variance", srt::JsonObject{{"predictions", std::move(predictions)}});
            }

            for (const char *parameter : {"energy", "breathiness", "voicing", "tension", "mouth_opening", "gender", "velocity"}) {
                if (!supported(parameter)) {
                    continue;
                }
                if (std::string_view(parameter) != "gender" && std::string_view(parameter) != "velocity" && std::ranges::find(record.variancePredictions, parameter) != record.variancePredictions.end()) {
                    std::vector<std::string> dependencies{"pitch"};
                    if (vocoderConfiguration->pitchControllable) {
                        dependencies.push_back("tone_shift");
                    }
                    group.parameterPipeline.emplace(parameter, arch::IndirectParameter{std::move(dependencies), arch::ParameterRetakeMode::Range});
                } else {
                    group.parameterPipeline.emplace(parameter, arch::DirectParameter{});
                }
                group.audioDependencies.push_back(parameter);
            }

            if (vocoderConfiguration->pitchControllable) {
                group.parameterPipeline.emplace("tone_shift", arch::DirectParameter{});
                group.audioDependencies.push_back("tone_shift");
            }

            record.technicallyMixable = completeLanguagePipeline && duration->spec && pitch->spec && variance->spec && duration->usesSpeakers && variance->usesSpeakers && acoustic->usesSpeakers && !record.speakers.empty();
            group.mixable = record.technicallyMixable && record.permitsMixing;

            // Effective imports include every language and every stage, with exact
            // resolved versions and recursive resources; public speaker selection
            // and host permissions do not alter this pipeline identity.
            std::map<std::string, std::string> roleNames;
            for (const auto &[handle, role] : singer.languages()) {
                roleNames.emplace(role, "language/" + handle);
                effectiveOptions.emplace(role, srt::JsonObject{});
            }

            // The pinned provider retains dict but does not consume it. Wolf's
            // effective language resources already occur in the binding graph.
            const auto description = srt::JsonValue(srt::JsonObject{{"pipeline", inference::describeImports(singer, effectiveOptions, roleNames)}, {"reservedPhonemes", srt::JsonArray{}}});
            auto identity = description.toObject();
            srt::JsonArray reserved;
            const std::set<std::string> reservedTokens(singer.reservedPhonemes().begin(), singer.reservedPhonemes().end());
            for (const auto &token : reservedTokens) {
                reserved.push_back(token);
            }

            identity["reservedPhonemes"] = std::move(reserved);
            srt::JsonObject languages;
            for (const auto &[handle, depth] : languageDepths) {
                languages.emplace(handle, depthName(depth));
            }

            identity["languages"] = std::move(languages);
            auto digest = inference::fingerprint(srt::JsonValue(std::move(identity)).toJson());
            if (!digest) {
                return std::unexpected{std::move(digest.error())};
            }

            group.id = "g1." + *digest;
            record.info.group = group.id;
            record.info.archSpecificInfo = Json::Value(Json::objectValue);
            record.info.archSpecificInfo["speakers"] = Json::Value(Json::arrayValue);
            for (const auto &speaker : record.speakers) {
                Json::Value info(Json::objectValue);
                info["id"] = speaker;
                info["name"] = speaker;
                record.info.archSpecificInfo["speakers"].append(std::move(info));
            }

            record.info.defaultExtra = Json::Value(Json::objectValue);
            record.info.defaultExtra["speaker"] = record.speakers.empty() ? std::string{} : record.speakers.front();
            log.stdcDebug("Cached singer %1 in group %2 (%3 languages, %4 speakers, %5 parameters, technical mixing=%6, mixing permission=%7)", record.info.id, group.id, group.languages.size(), record.speakers.size(), group.parameterPipeline.size(), record.technicallyMixable, record.permitsMixing);
            return std::make_pair(std::move(record), std::move(groupRecord));
        }
    }

    std::vector<std::string> commonSpeakers(const std::map<std::string, StageRecord> &stages) {
        std::optional<std::vector<std::string>> result;
        for (const auto &[role, stage] : stages) {
            if (!stage.usesSpeakers) {
                continue;
            }

            std::vector<std::string> available;
            if (stage.speakerMapping.empty()) {
                for (const auto &speaker : stage.speakers) {
                    if (stage.embeddedSpeakers.contains(speaker) && std::ranges::find(available, speaker) == available.end()) {
                        available.push_back(speaker);
                    }
                }
            } else {
                for (const auto &[logical, target] : stage.speakerMapping) {
                    if (stage.embeddedSpeakers.contains(target) && std::ranges::find(stage.speakers, target) != stage.speakers.end()) {
                        available.push_back(logical);
                    }
                }
            }

            if (!result) {
                result = std::move(available);
            } else {
                std::erase_if(*result, [&](const auto &speaker) { return std::ranges::find(available, speaker) == available.end(); });
            }
        }

        return result.value_or(std::vector<std::string>{});
    }

    arch::CatalogResult<arch::SingerInfo> singerInfo(const SingerRecord &record, const arch::CatalogOptions &display) {
        auto result = record.info;
        auto name = inference::localize(record.name, display.acceptLanguage, result.id);
        if (!name) {
            return fail<arch::CatalogErrors>(arch::OperationError("Cannot localize singer name", name.error().clone()));
        }

        result.name = std::move(*name);
        return result;
    }

    arch::CatalogResult<arch::GroupMetadata> groupInfo(const GroupRecord &record, const arch::CatalogOptions &display) {
        auto result = record.info;
        for (auto &[handle, language] : result.languages) {
            const auto name = record.languageNames.find(handle);
            if (name == record.languageNames.end()) {
                return fail<arch::CatalogErrors>(arch::OperationError("The cached group has no language name for " + handle));
            }

            auto title = inference::localize(name->second, display.acceptLanguage, handle);
            if (!title) {
                return fail<arch::CatalogErrors>(arch::OperationError("Cannot localize language name", title.error().clone()));
            }
            language.name = std::move(*title);
        }

        return result;
    }

    std::expected<std::shared_ptr<const SingerCatalog>, inference::ResourceError> SingerCatalog::discover(std::shared_ptr<inference::Environment> environment) {
        if (!environment) {
            return std::unexpected{inference::ResourceError("Singer discovery requires an inference environment")};
        }

        try {
            auto catalog = std::shared_ptr<SingerCatalog>(new SingerCatalog(std::move(environment)));
            std::size_t skipped = 0;
            for (const auto &package : catalog->m_environment->packages()) {
                for (const auto *contribution : package.contributions("singer")) {
                    if (contribution->interface() != Ds::API_INTERFACE || contribution->variant() != Ds::API_VARIANT || contribution->level() != Ds::API_LEVEL) {
                        log.stdcTrace("Ignoring singer contribution %1 in package %2 with a different architecture contract", contribution->locator().contributionId(), package.id());
                        continue;
                    }

                    const auto *singer = dynamic_cast<const srt::SingerSpec *>(contribution);
                    if (!singer) {
                        ++skipped;
                        log.stdcWarning("Skipping an invalid singer category declaration in %1", package.id());
                        continue;
                    }

                    auto found = readSinger(*singer);
                    if (!found) {
                        ++skipped;
                        log.stdcWarning("Skipping DiffSinger singer %1 in %2@%3: %4", singer->locator().contributionId(), package.id(), package.version().toString(), found.error().formatChain());
                        continue;
                    }

                    auto [record, group] = std::move(*found);
                    const bool mixable = group.info.mixable;
                    const auto [existing, inserted] = catalog->m_groups.try_emplace(record.info.group, std::move(group));
                    if (!inserted) {
                        existing->second.info.mixable = existing->second.info.mixable && mixable;
                    }

                    const auto id = record.info.id;
                    catalog->m_singers.emplace(id, std::move(record));
                }
            }

            log.stdcInfo("DiffSinger singer catalogue ready: %1 singers, %2 groups, %3 rejected declarations", catalog->m_singers.size(), catalog->m_groups.size(), skipped);
            return std::shared_ptr<const SingerCatalog>(std::move(catalog));
        } catch (const std::exception &error) {
            return std::unexpected{inference::ResourceError("Cannot read the loaded singer declarations", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"synthrt/Wolf/dsinfer", "discover singers", "exception"}))};
        }
    }

    const SingerRecord *SingerCatalog::find(std::string_view id) const {
        const auto found = m_singers.find(std::string(id));
        return found == m_singers.end() ? nullptr : &found->second;
    }
}
