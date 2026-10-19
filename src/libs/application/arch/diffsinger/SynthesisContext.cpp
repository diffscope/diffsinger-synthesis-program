#include "SynthesisContext.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dssp::arch_diffsinger {
    arch::SynthesisResult<SynthesisOptions> synthesisOptions(const Json::Value &extra) {
        if (!extra.isObject()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger archExtra must be an object"));
        }

        SynthesisOptions result;
        if (extra.isMember("steps")) {
            const auto &steps = extra["steps"];
            if (!steps.isObject()) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger steps must be an object"));
            }
            for (const auto &[stage, target] : {std::pair{"pitch", &result.steps.pitch}, std::pair{"variance", &result.steps.variance}, std::pair{"acoustic", &result.steps.acoustic}}) {
                if (!steps.isMember(stage)) {
                    continue;
                }
                const auto &value = steps[stage];
                if (!value.isIntegral() || !value.isInt64() || value.asInt64() <= 0) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger steps." + std::string(stage) + " must be a positive signed 64-bit integer"));
                }
                *target = value.asInt64();
            }
        }

        if (extra.isMember("depth")) {
            const auto &depth = extra["depth"];
            if (!depth.isNumeric() || !std::isfinite(depth.asDouble()) || depth.asDouble() < 0 || depth.asDouble() > 1) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger depth must be finite and between 0 and 1"));
            }
            result.depth = static_cast<float>(depth.asDouble());
        }

        return result;
    }

    arch::SynthesisResult<SingerSource> singerSource(const SingerCatalog &catalog, const arch::Singer &input) {
        const auto *singer = catalog.find(input.id);
        if (!singer) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("Singer does not exist: " + input.id));
        }
        if (!input.extra.isObject()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger extra must be an object for singer " + input.id));
        }

        auto speaker = singer->speakers.empty() ? std::string{} : singer->speakers.front();
        if (input.extra.isMember("speaker")) {
            const auto &value = input.extra["speaker"];
            if (!value.isString()) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger speaker must be a string for singer " + input.id));
            }
            speaker = value.asString();
        }

        if ((!speaker.empty() && std::ranges::find(singer->speakers, speaker) == singer->speakers.end()) || (speaker.empty() && !singer->speakers.empty())) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("Singer " + input.id + " has no selectable logical speaker '" + speaker + "'"));
        }

        return SingerSource{singer, std::move(speaker)};
    }

    arch::SynthesisResult<SynthesisContext> synthesisContext(const SingerCatalog &catalog, const arch::MultiSingerContext &input) {
        auto options = synthesisOptions(input.archExtra);
        if (!options) {
            return std::unexpected{std::move(options.error())};
        }
        if (input.singers.empty()) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("DiffSinger synthesis requires at least one singer"));
        }

        SynthesisContext result;
        result.options = *options;
        for (const auto &inputSinger : input.singers) {
            auto source = singerSource(catalog, inputSinger);
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
                return fail<arch::SynthesisErrors>(arch::InvalidInputError("Singer " + source->singer->info.id + " does not share the complete synthesis pipeline of group " + result.group->info.id));
            }
            result.sources.push_back(std::move(*source));
        }
        if (result.sources.size() > 1 && !result.group->info.mixable) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("Group " + result.group->info.id + " does not permit multiple singer or speaker sources"));
        }

        return result;
    }
}
