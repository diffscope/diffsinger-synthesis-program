#ifndef DSSP_EXTRACTION_TYPES_H
#define DSSP_EXTRACTION_TYPES_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <dsspextraction/ExtractionError.h>
#include <dssplocalization/Context.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::extraction {
    enum class Kind { Note, Pitch, Tempo, Separation };

    struct TrackInfo {
        std::string name;
        std::string description;
    };

    struct ExtractorInfo {
        std::string id;
        std::string name;
        std::uint32_t sampleRate = 0;
        std::vector<TrackInfo> tracks;
    };

    struct ExtractorList {
        std::vector<ExtractorInfo> note;
        std::vector<ExtractorInfo> tempo;
        std::vector<ExtractorInfo> pitch;
        std::vector<ExtractorInfo> separation;
    };

    struct Input {
        std::string extractor;
        std::string audioUrl;
        resource::TaskProgress progress;
        localization::Context locale;
    };

    struct SeparationInput : Input {
        std::vector<std::string> acceptableFormats;
        std::vector<std::string> acceptableSchemes;
    };

    struct ExtractedNote {
        double gap = 0;
        double duration = 0;
        std::uint32_t cent = 0;
    };

    struct NoteOutput {
        std::vector<ExtractedNote> notes;
    };

    struct ExtractedBeat {
        double position = 0;
        bool downbeat = false;
    };

    struct TempoOutput {
        std::vector<ExtractedBeat> beats;
    };

    struct PitchSegment {
        std::uint64_t gap = 0;
        std::vector<double> pitch;
    };

    struct PitchOutput {
        std::vector<PitchSegment> segments;
        double sampleRate = 0;
    };

    struct SeparationTrack {
        std::string name;
        std::string audioUrl;
    };

    struct SeparationOutput {
        std::vector<SeparationTrack> tracks;
    };

    template <class T> using Completion = std::function<void(ExtractionResult<T>)>;
}

#endif // DSSP_EXTRACTION_TYPES_H
