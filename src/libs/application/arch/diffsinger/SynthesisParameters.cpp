#include "SynthesisParameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <stdcorelib/support/logging.h>

namespace dssp::arch_diffsinger {
    namespace {
        stdc::LogCategory log("dssp.arch_diffsinger.SynthesisParameters");
        constexpr std::array definitions{
            ParameterDefinition{"pitch", "pitch", 0, 0, 128},
            ParameterDefinition{"expressiveness", "expr", 1, 0, 1},
            ParameterDefinition{"energy", "energy", 1, -96, 0},
            ParameterDefinition{"breathiness", "breathiness", 1, -96, 0},
            ParameterDefinition{"voicing", "voicing", 1, -96, 0},
            ParameterDefinition{"tension", "tension", 0.5, -10, 10},
            ParameterDefinition{"mouth_opening", "mouth_opening", 0, 0, 1},
            ParameterDefinition{"gender", "gender", 0.5, -1, 1},
            ParameterDefinition{"velocity", "velocity", 0.5, 0.5, 2},
            ParameterDefinition{"tone_shift", "tone_shift", 0.5, -1200, 1200}
        };
    }

    const ParameterDefinition *parameterDefinition(std::string_view id) {
        const auto found = std::ranges::find(definitions, id, &ParameterDefinition::id);
        return found == definitions.end() ? nullptr : &*found;
    }

    double engineValue(const ParameterDefinition &definition, double value) {
        if (definition.id == "pitch") {
            return value * 128;
        }
        if (definition.id == "energy" || definition.id == "breathiness" || definition.id == "voicing") {
            return -96 * std::pow(1 - value, 0.8);
        }
        if (definition.id == "tension") {
            const double axis = value * 2 - 1;
            return std::copysign(10 * (1 - std::pow(1 - std::abs(axis), 0.7)), axis);
        }
        if (definition.id == "gender") {
            return value * 2 - 1;
        }
        if (definition.id == "velocity") {
            return std::exp2(value * 2 - 1);
        }
        if (definition.id == "tone_shift") {
            return value * 2400 - 1200;
        }
        return value;
    }

    double normalizedValue(const ParameterDefinition &definition, double value) {
        if (definition.id == "pitch") {
            return value / 128;
        }
        if (definition.id == "energy" || definition.id == "breathiness" || definition.id == "voicing") {
            return 1 - std::pow(-value / 96, 1.25);
        }
        if (definition.id == "tension") {
            return 0.5 + std::copysign(0.5 * (1 - std::pow(1 - std::abs(value) / 10, 1 / 0.7)), value);
        }
        if (definition.id == "gender") {
            return (value + 1) / 2;
        }
        if (definition.id == "velocity") {
            return (std::log2(value) + 1) / 2;
        }
        if (definition.id == "tone_shift") {
            return (value + 1200) / 2400;
        }
        return value;
    }

    arch::SynthesisResult<std::size_t> sampleCount(double duration, double rate, const localization::Context &locale) {
        const double count = std::ceil(duration * rate);
        if (!std::isfinite(duration) || duration < 0 || !std::isfinite(rate) || rate <= 0 || !std::isfinite(1 / rate) || !std::isfinite(count) || count >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) || count >= static_cast<double>(std::vector<double>{}.max_size())) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.curve_sampling_grid_invalid", "Curve duration and sample rate do not define an addressable sampling grid")));
        }

        return static_cast<std::size_t>(count);
    }

    arch::SynthesisResult<Common::InputParameterInfo> parameterCurve(std::string_view id, std::span<const double> values, double rate, double duration, const localization::Context &locale) {
        const auto *definition = parameterDefinition(id);
        if (!definition) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.diffsinger_parameter_unknown", "Unknown DiffSinger parameter: {0}", id)));
        }

        auto count = sampleCount(duration, rate, locale);
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (std::ranges::any_of(values, [](double value) { return !std::isfinite(value) || value < 0 || value > 1; })) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.parameter_normalized_values_required", "Parameter {0} requires finite normalized values in [0,1]", id)));
        }

        Common::InputParameterInfo result;
        result.tag = ds::ParamTag(std::string(definition->tag));
        result.interval = 1 / rate;
        // Absent public retake is a condition, while native nullopt would recompute it.
        result.retake = Common::InputParameterInfo::RetakeRange{0, 0};
        result.values.assign(values.begin(), values.end());
        if (result.values.empty()) {
            result.values.push_back(definition->defaultValue);
        } else if (result.values.size() > 1 && result.values.size() <= *count) {
            // Include the endpoint needed to interpolate the last model frame.
            result.values.resize(*count + 1, definition->defaultValue);
        }

        for (auto &value : result.values) {
            value = engineValue(*definition, value);
        }

        return result;
    }

    arch::SynthesisResult<Common::InputParameterInfo::RetakeRange> parameterRetake(const arch::ParameterRetake &retake, double rate, double duration, const arch::IndirectParameter *capability, const localization::Context &locale) {
        auto count = sampleCount(duration, rate, locale);
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (retake.position > *count || retake.length > *count - retake.position) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("Retake exceeds the parameter's piece sampling grid"));
        }
        if (retake.length && !capability) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("A direct parameter cannot be recomputed"));
        }
        if (retake.length && capability->retakeMode == arch::ParameterRetakeMode::Full && (retake.position != 0 || retake.length != *count)) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError("This parameter supports only a full-piece retake"));
        }

        return Common::InputParameterInfo::RetakeRange{static_cast<double>(retake.position) / rate, static_cast<double>(retake.position + retake.length) / rate};
    }

    arch::SynthesisResult<arch::ParameterOutputParameter> parameterOutput(std::string_view id, std::span<const double> values, double interval, double duration) {
        const auto *definition = parameterDefinition(id);
        if (!definition || !std::isfinite(interval) || interval <= 0 || !std::isfinite(1 / interval)) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Inference returned an unknown parameter or invalid interval for " + std::string(id)));
        }

        const double frames = std::round(duration / interval);
        if (!std::isfinite(frames) || frames < 0 || frames != static_cast<double>(values.size())) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Inference returned an incorrect full-curve length for parameter " + std::string(id)));
        }

        arch::ParameterOutputParameter result{{}, 1 / interval};
        result.values.reserve(values.size());
        std::size_t clipped = 0;
        for (const auto value : values) {
            if (!std::isfinite(value)) {
                return fail<arch::SynthesisErrors>(arch::OperationError("Inference returned a nonfinite value for parameter " + std::string(id)));
            }
            const double bounded = std::clamp(value, definition->minimum, definition->maximum);
            clipped += bounded != value;
            result.values.push_back(std::clamp(normalizedValue(*definition, bounded), 0.0, 1.0));
        }

        if (clipped) {
            log.stdcWarning("Clipped %1 of %2 predicted values for parameter %3 to its public range", clipped, values.size(), id);
        }

        return result;
    }

    double curveValue(const Common::InputParameterInfo &curve, double time, bool preserveVoicing) {
        const double position = std::clamp(time / curve.interval, 0.0, static_cast<double>(curve.values.size() - 1));
        const auto left = static_cast<std::size_t>(position);
        const auto right = std::min(left + 1, curve.values.size() - 1);
        const auto nearest = static_cast<std::size_t>(std::round(position));
        if (preserveVoicing && curve.values[nearest] <= 0) {
            return 0;
        }
        return std::lerp(curve.values[left], curve.values[right], position - static_cast<double>(left));
    }

    arch::SynthesisResult<Common::InputParameterInfo> effectivePitch(const Common::InputParameterInfo &pitch, const Common::InputParameterInfo *toneShift, double duration, double interval) {
        auto count = sampleCount(duration, 1 / interval);
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }

        Common::InputParameterInfo result;
        result.tag = Common::Tags::Pitch;
        result.interval = interval;
        result.retake = Common::InputParameterInfo::RetakeRange{0, 0};
        const auto frames = static_cast<std::size_t>(std::round(duration / interval));
        // A zero-length model grid is handled without invoking the model.
        result.values.reserve(std::max<std::size_t>(frames, 1));
        for (std::size_t i = 0; i < frames; ++i) {
            const double time = static_cast<double>(i) * interval;
            const double original = curveValue(pitch, time, true);
            result.values.push_back(original <= 0 ? 0 : original + (toneShift ? curveValue(*toneShift, time) / 100 : 0));
        }

        if (result.values.empty()) {
            result.values.push_back(0);
        }

        return result;
    }

    arch::SynthesisResult<std::vector<float>> vocoderF0(const Common::InputParameterInfo &pitch, std::size_t frames, double interval) {
        if (!std::isfinite(interval) || interval <= 0 || pitch.values.empty() || !std::isfinite(pitch.interval) || pitch.interval <= 0) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Cannot align original singing pitch to the acoustic frame grid"));
        }

        std::vector<float> result;
        result.reserve(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const double value = curveValue(pitch, static_cast<double>(i) * interval, true);
            const double hz = value <= 0 ? 0 : 440 * std::exp2((value - 69) / 12);
            if (!std::isfinite(hz) || hz > std::numeric_limits<float>::max()) {
                return fail<arch::SynthesisErrors>(arch::OperationError("Original pitch produced a nonfinite vocoder frequency"));
            }
            result.push_back(static_cast<float>(hz));
        }

        return result;
    }
}
