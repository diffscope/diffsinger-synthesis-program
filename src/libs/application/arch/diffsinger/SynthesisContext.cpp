#include "SynthesisContext.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dssp::arch_diffsinger {
    arch::SynthesisResult<SynthesisOptions> synthesisOptions(const Json::Value &extra, const localization::Context &locale) {
        if (!extra.isObject()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_arch_extra_object", "DiffSinger archExtra must be an object")));
        }

        SynthesisOptions result;
        if (extra.isMember("steps")) {
            const auto &steps = extra["steps"];
            if (!steps.isObject()) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_steps_object", "DiffSinger steps must be an object")));
            }
            for (const auto &[stage, target] : {std::pair{"pitch", &result.steps.pitch}, std::pair{"variance", &result.steps.variance}, std::pair{"acoustic", &result.steps.acoustic}}) {
                if (!steps.isMember(stage)) {
                    continue;
                }
                const auto &value = steps[stage];
                if (!value.isIntegral() || !value.isInt64() || value.asInt64() <= 0) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_steps_positive_integer", "DiffSinger steps.{0} must be a positive signed 64-bit integer", stage)));
                }
                *target = value.asInt64();
            }
        }

        if (extra.isMember("depth")) {
            const auto &depth = extra["depth"];
            if (!depth.isNumeric() || !std::isfinite(depth.asDouble()) || depth.asDouble() < 0 || depth.asDouble() > 1) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_depth_range", "DiffSinger depth must be finite and between 0 and 1")));
            }
            result.depth = static_cast<float>(depth.asDouble());
        }

        return result;
    }

    arch::SynthesisResult<SingerSource> singerSource(const SingerCatalog &catalog, const arch::Singer &input, const localization::Context &locale) {
        const auto *singer = catalog.find(input.id);
        if (!singer) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.singer_not_found", "Singer does not exist: {0}", input.id)));
        }
        if (!input.extra.isObject()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_singer_extra_object", "DiffSinger extra must be an object for singer {0}", input.id)));
        }

        auto speaker = singer->speakers.empty() ? std::string{} : singer->speakers.front();
        if (input.extra.isMember("speaker")) {
            const auto &value = input.extra["speaker"];
            if (!value.isString()) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_speaker_string", "DiffSinger speaker must be a string for singer {0}", input.id)));
            }
            speaker = value.asString();
        }

        if ((!speaker.empty() && std::ranges::find(singer->speakers, speaker) == singer->speakers.end()) || (speaker.empty() && !singer->speakers.empty())) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_speaker_unavailable", "Singer {0} has no selectable logical speaker '{1}'", input.id, speaker)));
        }

        return SingerSource{singer, std::move(speaker)};
    }

    arch::SynthesisResult<SynthesisContext> synthesisContext(const SingerCatalog &catalog, const arch::MultiSingerContext &input) {
        const auto &locale = input.locale;
        auto options = synthesisOptions(input.archExtra, locale);
        if (!options) {
            return std::unexpected{std::move(options.error())};
        }
        if (input.singers.empty()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_singer_required", "DiffSinger synthesis requires at least one singer")));
        }

        SynthesisContext result;
        result.locale = locale;
        result.options = *options;
        for (const auto &inputSinger : input.singers) {
            auto source = singerSource(catalog, inputSinger, locale);
            if (!source) {
                return std::unexpected{std::move(source.error())};
            }
            if (!result.group) {
                const auto found = catalog.groups().find(source->singer->info.group);
                if (found == catalog.groups().end()) {
                    return fail<arch::SynthesisErrors>(arch::OperationError("Cached singer has no group: " + source->singer->info.id));
                }
                result.group = &found->second;
            } else if (result.group->info.id != source->singer->info.group) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_singer_pipeline_mismatch", "Singer {0} does not share the complete synthesis pipeline of group {1}", source->singer->info.id, result.group->info.id)));
            }
            result.sources.push_back(std::move(*source));
        }
        if (result.sources.size() > 1 && !result.group->info.mixable) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_group_mixing_unsupported", "Group {0} does not permit multiple singer or speaker sources", result.group->info.id)));
        }

        return result;
    }
}
