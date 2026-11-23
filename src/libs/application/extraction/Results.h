#ifndef DSSP_EXTRACTION_RESULTS_H
#define DSSP_EXTRACTION_RESULTS_H

#include <utility>

#include <dsspextraction/OtterTypes.h>
#include <dsspextraction/Types.h>

namespace dssp::extraction {
    std::expected<NoteOutput, OperationError> noteOutput(std::vector<otter::Api::Note::L1::Info>, double duration);
    class PitchBuilder {
    public:
        explicit PitchBuilder(double interval) : m_interval(interval) { m_output.sampleRate = 1.0 / interval; }
        std::expected<void, OperationError> append(const otter::Api::F0::L1::Result &, double spanStart, double spanEnd, int audioSampleRate);
        PitchOutput take() { return std::move(m_output); }

    private:
        double m_interval;
        std::uint64_t m_consumedEnd = 0;
        std::uint64_t m_voicedEnd = 0;
        PitchOutput m_output;
    };
}

#endif // DSSP_EXTRACTION_RESULTS_H
