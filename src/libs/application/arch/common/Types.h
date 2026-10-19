#ifndef DSSP_ARCH_TYPES_H
#define DSSP_ARCH_TYPES_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <json/value.h>

#include <dsspresource/TaskProgress.h>

namespace dssp::arch {

    // Domain inputs retain API units. Runtime-specific conversion and extension
    // interpretation belong to the architecture implementation.
    struct Singer {
        std::string id;
        Json::Value extra;
    };

    struct SingleSingerContext {
        Json::Value archExtra;
        Singer singer;
    };

    struct MultiSingerContext {
        Json::Value archExtra;
        std::vector<Singer> singers;
    };

    struct Lyric {
        std::string lyric;
        std::string language;
    };

    struct PronunciationNote {
        std::string pronunciation;
        std::string language;
        std::optional<std::int64_t> syllableSliceStart;
        std::optional<std::int64_t> syllableSliceEnd;
    };

    struct PronunciationInput {
        SingleSingerContext context;
        std::vector<Lyric> notes;
        resource::TaskProgress progress;
    };

    struct PhonemeInput {
        SingleSingerContext context;
        std::vector<PronunciationNote> notes;
        resource::TaskProgress progress;
    };

    enum class NoteKind {
        Normal,
        Slur
    };

    struct NotePosition {
        double gap = 0;
        double duration = 0;
    };

    struct DurationInputPhoneme {
        std::string token;
        bool onset = false;
        std::string language;
    };

    struct TimedPhoneme : DurationInputPhoneme {
        double start = 0;
    };

    template <class Phoneme>
    struct Note {
        NotePosition position;
        std::uint32_t cent = 0;
        std::string pronunciation;
        std::string language;
        std::vector<Phoneme> phonemes;
        NoteKind kind = NoteKind::Normal;
    };

    using DurationNote = Note<DurationInputPhoneme>;
    using ParameterNote = Note<TimedPhoneme>;
    template <class NoteType>
    struct Piece {
        double pieceDuration = 0;
        std::vector<NoteType> notes;
        std::vector<std::vector<double>> mix;
        double mixSampleRate = 1;
    };

    struct DurationInput {
        MultiSingerContext context;
        Piece<DurationNote> piece;
        resource::TaskProgress progress;
    };

    struct ParameterRetake {
        std::uint64_t position = 0;
        std::uint64_t length = 0;
    };

    struct Parameter {
        std::vector<double> values;
        double sampleRate = 1;
        std::optional<ParameterRetake> retake;
    };

    struct AudioParameter {
        double sampleRate = 1;
        std::optional<std::vector<double>> values;
    };

    struct MediaPreferences {
        std::vector<std::string> acceptableFormats;
        std::vector<std::string> acceptableSchemes;
    };

    struct ParameterInput {
        MultiSingerContext context;
        Piece<ParameterNote> piece;
        std::map<std::string, Parameter> parameters;
        resource::TaskProgress progress;
    };

    struct AudioInput {
        MultiSingerContext context;
        Piece<ParameterNote> piece;
        std::map<std::string, AudioParameter> parameters;
        MediaPreferences media;
        resource::TaskProgress progress;
    };

    struct CatalogOptions {
        std::string acceptLanguage;
    };

    struct ResourceQuery {
        std::string id;
        CatalogOptions display;
    };

    struct SingerMediaQuery {
        ResourceQuery singer;
        MediaPreferences media;
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

    struct SingerDemoAudio {
        std::string name;
        std::string audioUrl;
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

    using PronunciationOutput = NotesOutput<Pronunciation>;
    using PhonemeOutput = NotesOutput<PhonemeNote>;
    using DurationOutput = NotesOutput<DurationOutputNote>;
    struct ParameterOutputParameter {
        std::vector<double> values;
        double sampleRate = 1;
    };

    struct ParameterOutput {
        std::map<std::string, ParameterOutputParameter> parameters;
    };

    struct AudioOutput {
        std::string audioUrl;
    };

}

#endif // DSSP_ARCH_TYPES_H
