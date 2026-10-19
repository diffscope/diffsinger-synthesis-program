#include "ArchitectureMapping.h"

#include <variant>

#include <dssperror/ErrorSet.h>

namespace dssp::api_v1 {
    namespace {
        arch::NoteKind noteKind(dto::NoteKind kind) {
            switch (kind) {
                case dto::NoteKind::Normal: return arch::NoteKind::Normal;
                case dto::NoteKind::Slur: return arch::NoteKind::Slur;
            }
            return arch::NoteKind::Normal;
        }

        template <class Target, class Source>
        Target note(Source value) {
            return {{value.position.gap, value.position.duration}, value.cent, std::move(value.pronunciation), std::move(value.language), toArchitecture(std::move(value.phonemes)), noteKind(value.kind)};
        }

        template <class TargetNote, class SourceNote>
        arch::Piece<TargetNote> piece(dto::PieceInput<SourceNote> value) {
            return {value.pieceDuration, toArchitecture(std::move(value.notes)), std::move(value.mix), value.mixSampleRate};
        }
    }

    arch::Singer toArchitecture(dto::Singer value) {
        return {std::move(value.id), std::move(value.extra)};
    }

    arch::SingleSingerContext toArchitecture(dto::SingleSingerContext value) {
        return {std::move(value.archExtra), toArchitecture(std::move(value.singer))};
    }

    arch::MultiSingerContext toArchitecture(dto::MultiSingerContext value) {
        return {std::move(value.archExtra), toArchitecture(std::move(value.singers))};
    }

    arch::Lyric toArchitecture(dto::Lyric value) {
        return {std::move(value.lyric), std::move(value.language)};
    }

    arch::PronunciationNote toArchitecture(dto::PronunciationNote value) {
        return {std::move(value.pronunciation), std::move(value.language), value.syllableSliceStart, value.syllableSliceEnd};
    }

    arch::DurationInputPhoneme toArchitecture(dto::DurationInputPhoneme value) {
        return {std::move(value.token), value.onset, std::move(value.language)};
    }

    arch::TimedPhoneme toArchitecture(dto::ParameterInputPhoneme value) {
        return {toArchitecture(static_cast<dto::DurationInputPhoneme &&>(value)), value.start};
    }

    arch::DurationNote toArchitecture(dto::DurationNote value) {
        return note<arch::DurationNote>(std::move(value));
    }

    arch::ParameterNote toArchitecture(dto::ParameterNote value) {
        return note<arch::ParameterNote>(std::move(value));
    }

    arch::MultiSingerContext toArchitecture(dto::EnvTagRequest value) {
        return toArchitecture(std::move(value.context));
    }

    arch::PronunciationInput toArchitecture(dto::PronunciationRequest value) {
        return {toArchitecture(std::move(value.context)), toArchitecture(std::move(value.input.notes))};
    }

    arch::PhonemeInput toArchitecture(dto::PhonemeRequest value) {
        return {toArchitecture(std::move(value.context)), toArchitecture(std::move(value.input.notes))};
    }

    arch::DurationInput toArchitecture(dto::DurationRequest value) {
        return {toArchitecture(std::move(value.context)), piece<arch::DurationNote>(std::move(value.input))};
    }

    arch::ParameterInput toArchitecture(dto::ParameterRequest value) {
        arch::ParameterInput result{toArchitecture(std::move(value.context)), piece<arch::ParameterNote>(static_cast<dto::PieceInput<dto::ParameterNote> &&>(value.input)), {}};
        for (auto &[key, parameter] : value.input.parameters) {
            std::optional<arch::ParameterRetake> retake;
            if (parameter.retake) {
                retake = arch::ParameterRetake{parameter.retake->position, parameter.retake->length};
            }
            result.parameters.emplace(key, arch::Parameter{std::move(parameter.values), parameter.sampleRate, retake});
        }

        return result;
    }

    arch::AudioInput toArchitecture(dto::AudioRequest value) {
        arch::AudioInput result{
            toArchitecture(std::move(value.context)),
            piece<arch::ParameterNote>(static_cast<dto::PieceInput<dto::ParameterNote> &&>(value.input)),
            {},
            {value.acceptableFormats ? std::move(*value.acceptableFormats) : std::vector<std::string>{},
                value.acceptableSchemes ? std::move(*value.acceptableSchemes) : std::vector<std::string>{}}};
        for (auto &[key, parameter] : value.input.parameters) {
            result.parameters.emplace(key, arch::AudioParameter{parameter.sampleRate, std::move(parameter.values)});
        }

        return result;
    }

    arch::MediaPreferences toArchitecture(dto::MediaQuery value) {
        return {std::move(value.acceptableFormats), std::move(value.acceptableSchemes)};
    }

    dto::AST toDto(arch::AST value) {
        return std::visit(Overload{[](double number) -> dto::AST { return {number}; }, [](arch::AST::Operation operation) -> dto::AST { return {dto::AST::Operation{std::move(operation.op), toDto(std::move(operation.operands))}}; }}, std::move(value.value));
    }

    dto::ParameterPresentation toDto(arch::ParameterPresentation value) {
        dto::ParameterPresentation result;
        result.name = std::move(value.name);
        result.defaultValue = value.defaultValue;
        result.showBaseline = value.showBaseline;
        result.baselineValue = value.baselineValue;
        switch (value.fillMode) {
            case arch::FillMode::No: result.fillMode = dto::FillMode::No; break;
            case arch::FillMode::Top: result.fillMode = dto::FillMode::Top; break;
            case arch::FillMode::Bottom: result.fillMode = dto::FillMode::Bottom; break;
            case arch::FillMode::Baseline: result.fillMode = dto::FillMode::Baseline; break;
        }
        result.fallbackOnDefaultValue = value.fallbackOnDefaultValue;
        result.displayValueMappingExpression = toDto(std::move(value.displayValueMappingExpression));
        result.displayValueInverseMappingExpression = toDto(std::move(value.displayValueInverseMappingExpression));
        result.displayValuePrefix = std::move(value.displayValuePrefix);
        result.displayValueSuffix = std::move(value.displayValueSuffix);
        result.displayValueDecimalPlaces = value.displayValueDecimalPlaces;
        return result;
    }

    dto::ArchitectureMetadata toDto(arch::ArchitectureMetadata value) {
        return {std::move(value.id), std::move(value.name), toDto(std::move(value.parameters)), std::move(value.defaultArchExtra)};
    }

    dto::GroupMetadata toDto(arch::GroupMetadata value) {
        dto::GroupMetadata result;
        result.id = std::move(value.id);
        result.arch = std::move(value.arch);
        result.languages = toDto(std::move(value.languages));
        switch (value.durationMode) {
            case arch::DurationMode::Full: result.durationMode = dto::DurationMode::Full; break;
            case arch::DurationMode::Skip: result.durationMode = dto::DurationMode::Skip; break;
        }

        using Pipeline = std::variant<dto::DirectParameter, dto::IndirectParameter>;
        const auto visitor = Overload{
            [](arch::DirectParameter) -> Pipeline { return dto::DirectParameter{}; },
            [](arch::IndirectParameter indirect) -> Pipeline {
                dto::IndirectParameter result{std::move(indirect.dependsOn)};
                switch (indirect.retakeMode) {
                    case arch::ParameterRetakeMode::Full: result.retakeMode = dto::ParameterRetakeMode::Full; break;
                    case arch::ParameterRetakeMode::Range: result.retakeMode = dto::ParameterRetakeMode::Range; break;
                }
                return result;
            }};
        for (auto &[key, pipeline] : value.parameterPipeline) {
            result.parameterPipeline.emplace(key, std::visit(visitor, std::move(pipeline)));
        }
        result.audioDependencies = std::move(value.audioDependencies);
        result.mixable = value.mixable;
        result.archSpecificInfo = std::move(value.archSpecificInfo);
        return result;
    }

    dto::GroupLanguageInfo toDto(arch::GroupLanguageInfo value) {
        dto::GroupLanguageInfo result;
        result.name = std::move(value.name);
        result.defaultLyric = std::move(value.defaultLyric);
        switch (value.pronunciationMode) {
            case arch::PronunciationMode::Full: result.pronunciationMode = dto::PronunciationMode::Full; break;
            case arch::PronunciationMode::Skip: result.pronunciationMode = dto::PronunciationMode::Skip; break;
        }
        switch (value.phonemeMode) {
            case arch::PhonemeMode::Full: result.phonemeMode = dto::PhonemeMode::Full; break;
            case arch::PhonemeMode::TokenOnly: result.phonemeMode = dto::PhonemeMode::TokenOnly; break;
            case arch::PhonemeMode::Skip: result.phonemeMode = dto::PhonemeMode::Skip; break;
        }
        return result;
    }

    dto::SingerInfo toDto(arch::SingerInfo value) {
        return {std::move(value.id), std::move(value.name), std::move(value.arch), std::move(value.group), std::move(value.defaultLanguage), std::move(value.archSpecificInfo), std::move(value.defaultExtra)};
    }

    dto::SingerDemoAudio toDto(arch::SingerDemoAudio value) {
        return {std::move(value.name), std::move(value.audioUrl)};
    }

    dto::Pronunciation toDto(arch::Pronunciation value) {
        return {std::move(value.pronunciation), std::move(value.candidates)};
    }

    dto::Phoneme toDto(arch::Phoneme value) {
        return {std::move(value.token), value.onset};
    }

    dto::PhonemeNote toDto(arch::PhonemeNote value) {
        return {toDto(std::move(value.phonemes))};
    }

    dto::DurationOutputPhoneme toDto(arch::DurationOutputPhoneme value) {
        return {value.start};
    }

    dto::DurationOutputNote toDto(arch::DurationOutputNote value) {
        return {toDto(std::move(value.phonemes))};
    }

    dto::ParameterOutputParameter toDto(arch::ParameterOutputParameter value) {
        return {std::move(value.values), value.sampleRate};
    }

    dto::ParameterOutput toDto(arch::ParameterOutput value) {
        return {toDto(std::move(value.parameters))};
    }

    dto::AudioOutput toDto(arch::AudioOutput value) {
        return {std::move(value.audioUrl)};
    }

}
