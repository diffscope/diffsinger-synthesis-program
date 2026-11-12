#ifndef DSSP_ARCHDIFFSINGER_SYNTHESISPARAMETERS_H
#define DSSP_ARCHDIFFSINGER_SYNTHESISPARAMETERS_H

#include <span>
#include <string_view>

#include <dssparchdiffsinger/SynthesisScore.h>
#include <dssplocalization/Context.h>

namespace dssp::arch_diffsinger {
    struct ParameterDefinition {
        std::string_view id;
        std::string_view tag;
        double defaultValue;
        double minimum;
        double maximum;
    };

    const ParameterDefinition *parameterDefinition(std::string_view);
    double engineValue(const ParameterDefinition &, double normalized);
    double normalizedValue(const ParameterDefinition &, double engine);
    arch::SynthesisResult<std::size_t> sampleCount(double duration, double rate, const localization::Context &locale = {});
    arch::SynthesisResult<Common::InputParameterInfo> parameterCurve(std::string_view, std::span<const double>, double rate, double duration, const localization::Context &locale = {});
    arch::SynthesisResult<Common::InputParameterInfo::RetakeRange> parameterRetake(const arch::ParameterRetake &, double rate, double duration, const arch::IndirectParameter *, const localization::Context &locale = {});
    arch::SynthesisResult<arch::ParameterOutputParameter> parameterOutput(std::string_view, std::span<const double>, double interval, double duration);
    double curveValue(const Common::InputParameterInfo &, double time, bool preserveVoicing = false);
    arch::SynthesisResult<Common::InputParameterInfo> effectivePitch(const Common::InputParameterInfo &, const Common::InputParameterInfo *toneShift, double duration, double interval);
    arch::SynthesisResult<std::vector<float>> vocoderF0(const Common::InputParameterInfo &, std::size_t frames, double interval);
}

#endif // DSSP_ARCHDIFFSINGER_SYNTHESISPARAMETERS_H
