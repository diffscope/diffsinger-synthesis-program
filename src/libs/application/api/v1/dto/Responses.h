#ifndef DSSP_API_V1_DTO_RESPONSES_H
#define DSSP_API_V1_DTO_RESPONSES_H

#include <variant>

#include <dsspapiv1/Requests.h>

namespace dssp::api_v1::dto {

    struct ApiVersion {
        double major = 0;
        double minor = 0;
    };

    struct ApplicationInfo {
        ApiVersion apiVersion;
    };

    struct ApplicationInfoResponse {
        ApplicationInfo dssp;
    };

    struct AST {
        struct Operation {
            std::string op;
            std::vector<AST> operands;
        };
        std::variant<double, Operation> value = 0.0;
    };

    enum class FillMode {
        No,
        Top,
        Bottom,
        Baseline
    };

    struct ParameterPresentation {
        std::string name;
        double defaultValue = 0;
        bool showBaseline = false;
        double baselineValue = 0;
        FillMode fillMode = FillMode::No;
        bool fallbackOnDefaultValue = false;
        AST displayValueMappingExpression;
        AST displayValueInverseMappingExpression;
        std::string displayValuePrefix;
        std::string displayValueSuffix;
        unsigned displayValueDecimalPlaces = 0;
    };

    struct ArchitectureMetadata {
        std::string id;
        std::string name;
        std::map<std::string, ParameterPresentation> parameters;
        Json::Value defaultArchExtra;
    };

    enum class PronunciationMode {
        Full,
        Skip
    };

    enum class PhonemeMode {
        Full,
        TokenOnly,
        Skip
    };

    enum class DurationMode {
        Full,
        Skip
    };

    enum class ParameterRetakeMode {
        Full,
        Range
    };

    struct DirectParameter {};

    struct IndirectParameter {
        std::vector<std::string> dependsOn;
        ParameterRetakeMode retakeMode = ParameterRetakeMode::Full;
    };

    struct GroupLanguageInfo {
        std::string name;
        std::string defaultLyric;
        PronunciationMode pronunciationMode = PronunciationMode::Skip;
        PhonemeMode phonemeMode = PhonemeMode::Skip;
    };

    struct GroupMetadata {
        std::string id;
        std::string arch;
        std::map<std::string, GroupLanguageInfo> languages;
        DurationMode durationMode = DurationMode::Skip;
        std::map<std::string, std::variant<DirectParameter, IndirectParameter>> parameterPipeline;
        std::vector<std::string> audioDependencies;
        bool mixable = false;
        Json::Value archSpecificInfo;
    };

    struct SingerInfo {
        std::string id;
        std::string name;
        std::string arch;
        std::string group;
        std::string defaultLanguage;
        Json::Value archSpecificInfo;
        Json::Value defaultExtra;
    };

    struct SingerAvatarResponse {
        std::string avatarUrl;
    };

    struct SingerBackgroundResponse {
        std::string backgroundUrl;
    };

    struct SingerDemoAudio {
        std::string name;
        std::string audioUrl;
    };

    struct ExtractorInfo {
        std::string id;
        std::string name;
        std::uint32_t preferredAudioSampleRate = 1;
        std::optional<std::vector<std::string>> acceptableFormats;
        std::optional<std::vector<std::string>> acceptableSchemes;
        std::optional<std::vector<std::uint32_t>> acceptableAudioSampleRates;
    };

    struct SeparationTrackInfo {
        std::string name;
    };

    struct SeparationExtractorInfo : ExtractorInfo {
        std::vector<SeparationTrackInfo> tracks;
    };

    struct ExtractorList {
        std::vector<ExtractorInfo> note;
        std::vector<ExtractorInfo> tempo;
        std::vector<ExtractorInfo> pitch;
        std::vector<SeparationExtractorInfo> separation;
    };

    struct EnvTagResponse {
        std::string envTag;
    };

    template <class Output>
    struct Response {
        Output output;
    };

    struct Pronunciation {
        std::string pronunciation;
        std::vector<std::string> candidates;
    };

    struct Phoneme {
        std::string token;
        bool onset = false;
    };

    struct PhonemeNote {
        std::vector<Phoneme> phonemes;
    };

    struct DurationOutputPhoneme {
        double start = 0;
    };

    struct DurationOutputNote {
        std::vector<DurationOutputPhoneme> phonemes;
    };

    template <class NoteType>
    struct NotesOutput {
        std::vector<NoteType> notes;
    };
    using PronunciationResponse = Response<NotesOutput<Pronunciation>>;
    using PhonemeResponse = Response<NotesOutput<PhonemeNote>>;
    using DurationResponse = Response<NotesOutput<DurationOutputNote>>;
    struct ParameterOutputParameter {
        std::vector<double> values;
        double sampleRate = 1;
    };

    struct ParameterOutput {
        std::map<std::string, ParameterOutputParameter> parameters;
    };
    using ParameterResponse = Response<ParameterOutput>;
    struct AudioOutput {
        std::string audioUrl;
    };
    using AudioResponse = Response<AudioOutput>;
    struct ExtractedNote {
        NotePosition position;
        std::uint32_t cent = 0;
    };
    using NoteExtractionResponse = Response<NotesOutput<ExtractedNote>>;
    struct PitchExtractionSegment {
        std::uint64_t gap = 0;
        std::vector<double> pitch;
    };

    struct PitchExtractionOutput {
        std::vector<PitchExtractionSegment> segments;
        double sampleRate = 1;
    };
    using PitchExtractionResponse = Response<PitchExtractionOutput>;
    struct ExtractedBeat {
        double position = 0;
        bool downbeat = false;
    };

    struct TempoExtractionOutput {
        std::vector<ExtractedBeat> beats;
    };
    using TempoExtractionResponse = Response<TempoExtractionOutput>;
    struct SeparationTrack {
        std::string name;
        std::string audioUrl;
    };

    struct SeparationExtractionOutput {
        std::vector<SeparationTrack> tracks;
    };
    using SeparationExtractionResponse = Response<SeparationExtractionOutput>;

}

#endif // DSSP_API_V1_DTO_RESPONSES_H
