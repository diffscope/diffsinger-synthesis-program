#include "Results.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dssp::extraction {
    std::expected<NoteOutput, OperationError> noteOutput(std::vector<otter::Api::Note::L1::Info> notes, double duration) {
        constexpr double tolerance = 1e-5;
        if (!std::isfinite(duration) || duration < 0)
            return std::unexpected{OperationError("Invalid note result timeline")};
        // Establish finite sort keys before sorting results from different spans.
        for (const auto &note : notes) {
            if (!std::isfinite(note.start) || !std::isfinite(note.duration) || !std::isfinite(note.confidence) || note.confidence < 0 || note.confidence > 1 || note.key < 0 || note.key > 128 || note.duration < -tolerance || note.start < -tolerance || note.start + note.duration > duration + tolerance) {
                return std::unexpected{OperationError("Note analyzer returned an invalid key, timing or confidence")};
            }
        }

        std::ranges::stable_sort(notes, {}, &otter::Api::Note::L1::Info::start);
        NoteOutput output;
        output.notes.reserve(notes.size());
        double previousEnd = 0;
        for (std::size_t index = 0; index < notes.size(); ++index) {
            const auto &note = notes[index];
            if (note.start < previousEnd - tolerance) {
                return std::unexpected{OperationError("Note analyzer returned overlapping notes at index " + std::to_string(index))};
            }

            const auto start = std::clamp(std::max(note.start, previousEnd), 0.0, duration);
            const auto end = std::clamp(note.start + std::max(0.0, note.duration), start, duration);
            output.notes.push_back({start - previousEnd, end - start, static_cast<std::uint32_t>(note.key * 100)});
            previousEnd = end;
        }

        return output;
    }

    std::expected<void, OperationError> PitchBuilder::append(const otter::Api::F0::L1::Result &curve, double spanStart, double spanEnd, int audioSampleRate) {
        if (!std::isfinite(m_interval) || m_interval <= 0 || !std::isfinite(m_output.sampleRate) || !std::isfinite(curve.interval) || curve.interval <= 0 || std::abs(curve.interval - m_interval) > m_interval * 1e-9 || !std::isfinite(curve.startTime) || !std::isfinite(spanStart) || !std::isfinite(spanEnd) || spanStart < 0 || spanEnd <= spanStart || audioSampleRate <= 0) {
            return std::unexpected{OperationError("F0 analyzer returned an invalid or inconsistent output frame rate or timeline")};
        }

        const auto tolerance = 0.51 / audioSampleRate + 1e-9;
        const auto firstFrame = std::round(curve.startTime / m_interval);
        if (std::abs(curve.startTime - spanStart) > tolerance || firstFrame < 0 || firstFrame > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2) || std::abs(firstFrame * m_interval - curve.startTime) > tolerance) {
            return std::unexpected{OperationError("F0 analyzer result is not anchored to the input span and global output grid")};
        }
        if (!curve.voiced.empty() && curve.voiced.size() != curve.f0.size()) {
            return std::unexpected{OperationError("F0 analyzer returned different pitch and voiced-mask lengths")};
        }

        const auto first = static_cast<std::uint64_t>(firstFrame);
        if (first < m_consumedEnd)
            return std::unexpected{OperationError("F0 analyzer returned overlapping output frames")};
        const auto expected = std::ceil((spanEnd - spanStart) / m_interval - 1e-9);
        if (static_cast<double>(curve.f0.size()) > expected + 1) {
            return std::unexpected{OperationError("F0 analyzer returned frames beyond the input span")};
        }

        for (std::size_t index = 0; index < curve.f0.size(); ++index) {
            const auto frame = first + index;
            // A centered model window can return one padded boundary frame.
            // Keep the requested half-open span; the next span owns that frame.
            if (static_cast<double>(frame) * m_interval >= spanEnd - 1e-9)
                break;
            const auto mask = curve.voiced.empty() ? (curve.f0[index] > 0 ? 1 : 0) : curve.voiced[index];
            if (mask > 1)
                return std::unexpected{OperationError("F0 analyzer returned a nonboolean voiced mask")};
            m_consumedEnd = frame + 1;
            if (!mask)
                continue;
            const double frequency = curve.f0[index];
            if (!std::isfinite(frequency) || frequency <= 0)
                return std::unexpected{OperationError("F0 analyzer returned a nonpositive or nonfinite voiced frequency")};
            const auto cent = 100.0 * (69.0 + 12.0 * std::log2(frequency / 440.0));
            if (!std::isfinite(cent) || cent < 0 || cent > 12800)
                return std::unexpected{OperationError("F0 analyzer returned a voiced pitch outside [0,12800] cent")};
            if (!m_output.segments.empty() && frame == m_voicedEnd) {
                m_output.segments.back().pitch.push_back(cent);
            } else {
                m_output.segments.push_back({frame - m_voicedEnd, {cent}});
            }
            m_voicedEnd = frame + 1;
        }

        return {};
    }
}
