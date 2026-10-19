#include "Metadata.h"

#include <map>
#include <utility>

#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/SynthesisContext.h>

namespace dssp::arch_diffsinger {
    namespace {
        arch::AST number(double value) { return arch::AST{value}; }

        arch::AST op(std::string name, std::vector<arch::AST> operands = {}) { return arch::AST{arch::AST::Operation{std::move(name), std::move(operands)}}; }

        arch::AST x() { return op("x"); }

        arch::AST signedAxis() { return op("-", {op("*", {number(2), x()}), number(1)}); }

        arch::AST powerCurve(arch::AST value, double power) { return op("-", {number(1), op("pow", {op("-", {number(1), std::move(value)}), number(power)})}); }

        arch::ParameterPresentation presentation(std::string title, double value, arch::FillMode fill, bool fallback, arch::AST mapping, arch::AST inverse, std::string suffix = {}, bool baseline = false, unsigned decimals = 3) {
            return {std::move(title), value, baseline, value, fill, fallback, std::move(mapping), std::move(inverse), {}, std::move(suffix), decimals};
        }
    }

    arch::CatalogResult<arch::ArchitectureMetadata> architectureMetadata(const arch::CatalogOptions &display, const localization::Catalog &translations) {
        using arch::FillMode;
        std::map<std::string, std::string> text;
        for (const auto key : {"architecture", "pitch", "expressiveness", "energy", "breathiness", "voicing", "tension", "mouth_opening", "gender", "velocity", "tone_shift", "unitCent", "unitDecibel"}) {
            auto translated = translations.text(key, display.acceptLanguage);
            if (!translated) {
                return fail<arch::CatalogErrors>(arch::OperationError("Cannot localize DiffSinger parameter metadata", translated.error().clone()));
            }
            text.emplace(key, std::move(*translated));
        }

        arch::ArchitectureMetadata result{std::string(Id), text.at("architecture"), {}, Json::Value(Json::objectValue)};
        const SynthesisOptions options;
        result.defaultArchExtra["steps"]["pitch"] = options.steps.pitch;
        result.defaultArchExtra["steps"]["variance"] = options.steps.variance;
        result.defaultArchExtra["steps"]["acoustic"] = options.steps.acoustic;
        result.defaultArchExtra["depth"] = options.depth;
        result.parameters.emplace("pitch", presentation(text.at("pitch"), 0, FillMode::No, false, op("*", {number(12800), x()}), op("/", {x(), number(12800)}), text.at("unitCent")));
        result.parameters.emplace("expressiveness", presentation(text.at("expressiveness"), 1, FillMode::Bottom, true, x(), x()));
        for (const auto id : {"energy", "breathiness", "voicing"}) {
            result.parameters.emplace(id, presentation(text.at(id), 1, FillMode::Bottom, false, op("*", {number(-96), op("pow", {op("-", {number(1), x()}), number(0.8)})}), op("-", {number(1), op("pow", {op("/", {op("-", {number(0), x()}), number(96)}), number(1.25)})}), text.at("unitDecibel")));
        }

        auto tension = op("*", {number(10), op("sign", {signedAxis()}), powerCurve(op("abs", {signedAxis()}), 0.7)});
        auto inverseTension = op("+", {number(0.5), op("*", {number(0.5), op("sign", {x()}), powerCurve(op("/", {op("abs", {x()}), number(10)}), 1.0 / 0.7)})});
        result.parameters.emplace("tension", presentation(text.at("tension"), 0.5, FillMode::Bottom, false, std::move(tension), std::move(inverseTension)));
        result.parameters.emplace("mouth_opening", presentation(text.at("mouth_opening"), 0, FillMode::Bottom, false, x(), x()));
        result.parameters.emplace("gender", presentation(text.at("gender"), 0.5, FillMode::Baseline, true, signedAxis(), op("/", {op("+", {x(), number(1)}), number(2)}), {}, true));
        result.parameters.emplace("velocity", presentation(text.at("velocity"), 0.5, FillMode::Baseline, true, op("pow", {number(2), signedAxis()}), op("/", {op("+", {op("/", {op("ln", {x()}), op("ln", {number(2)})}), number(1)}), number(2)}), {}, true));
        result.parameters.emplace("tone_shift", presentation(text.at("tone_shift"), 0.5, FillMode::Baseline, true, op("-", {op("*", {number(2400), x()}), number(1200)}), op("/", {op("+", {x(), number(1200)}), number(2400)}), text.at("unitCent"), true, 0));
        return result;
    }
}
