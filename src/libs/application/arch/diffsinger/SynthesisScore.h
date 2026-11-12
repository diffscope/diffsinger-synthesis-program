#ifndef DSSP_ARCHDIFFSINGER_SYNTHESISSCORE_H
#define DSSP_ARCHDIFFSINGER_SYNTHESISSCORE_H

#include <optional>
#include <span>

#include <dsinfer/Api/Inferences/Common/1/CommonApiL1.h>

#include <dssparchdiffsinger/SynthesisContext.h>
#include <dssplocalization/Context.h>

namespace dssp::arch_diffsinger {
    namespace Common = ds::Api::Common::L1;
    struct PhonemeOrigin {
        std::size_t note = 0;
        std::size_t phoneme = 0;
    };

    struct WordOrigins {
        double start = 0;
        std::vector<std::optional<PhonemeOrigin>> phonemes;
    };

    struct SynthesisScore {
        std::vector<Common::InputWordInfo> words;
        std::vector<WordOrigins> origins;
        std::vector<double> noteStarts;
        std::vector<std::size_t> phonemeCounts;
        bool silent = true;
    };

    struct StageScore {
        std::vector<Common::InputWordInfo> words;
        std::vector<Common::InputSpeakerInfo> speakers;
    };

    arch::SynthesisResult<SynthesisScore> synthesisScore(const SingerRecord &, const arch::Piece<arch::DurationNote> &, const localization::Context &locale = {});
    arch::SynthesisResult<SynthesisScore> synthesisScore(const SingerRecord &, const arch::Piece<arch::ParameterNote> &, const localization::Context &locale = {});
    arch::SynthesisResult<void> validateMix(const SynthesisContext &, std::span<const std::vector<double>>, double rate);
    arch::SynthesisResult<StageScore> stageScore(const SynthesisScore &, const SynthesisContext &, std::string_view stage, std::span<const std::vector<double>> mix, double rate);
    arch::SynthesisResult<arch::DurationOutput> durationOutput(const SynthesisScore &, std::span<const double> durations);
}

#endif // DSSP_ARCHDIFFSINGER_SYNTHESISSCORE_H
