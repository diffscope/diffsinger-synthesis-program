#ifndef DSSP_ARCHDIFFSINGER_SYNTHESISINFERENCES_H
#define DSSP_ARCHDIFFSINGER_SYNTHESISINFERENCES_H

#include <dssparchdiffsinger/SynthesisScore.h>
#include <dssplocalization/Context.h>

namespace dssp::arch_diffsinger {
    enum class SynthesisStage {
        Duration,
        Pitch,
        Variance,
        Acoustic,
        Vocoder
    };

    std::string_view stageName(SynthesisStage);
    std::string synthesisCacheGroup(SynthesisStage);
    std::expected<std::string, inference::ExecutionError> synthesisInferenceKey(const SingerRecord &, SynthesisStage);
    std::expected<inference::InferenceHandle, inference::ExecutionError> bindSynthesis(inference::InferencePool &, const SingerRecord &, SynthesisStage);
    arch::SynthesisErrors synthesisExecutionError(const inference::ExecutionErrors &, std::string_view stage, const SingerRecord &);
    arch::SynthesisResult<void> validateStageWords(const SingerRecord &, SynthesisStage, const std::vector<Common::InputWordInfo> &, const localization::Context &);
    arch::SynthesisResult<void> validateMelConfiguration(const SingerRecord &);
}

#endif // DSSP_ARCHDIFFSINGER_SYNTHESISINFERENCES_H
