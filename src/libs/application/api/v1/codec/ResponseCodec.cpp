#include "ResponseCodec.h"

#include <dssperror/ErrorSet.h>

namespace dssp::api_v1 {

    Json::Value toJson(const dto::ApiVersion &value) {
        Json::Value result;
        result["major"] = value.major;
        result["minor"] = value.minor;
        return result;
    }

    Json::Value toJson(const dto::ApplicationInfo &value) {
        Json::Value result;
        result["apiVersion"] = toJson(value.apiVersion);
        return result;
    }

    Json::Value toJson(const dto::ApplicationInfoResponse &value) {
        Json::Value result;
        result["dssp"] = toJson(value.dssp);
        return result;
    }

    Json::Value toJson(const dto::AST &value) {
        const auto visitor = Overload{
            [](double number) -> Json::Value { return number; },
            [](const dto::AST::Operation &op) -> Json::Value {
                Json::Value result;
                result["operator"] = op.op;
                result["operands"] = toJson(op.operands);
                return result;
            }};
        return std::visit(visitor, value.value);
    }

    Json::Value toJson(const dto::ParameterPresentation &value) {
        Json::Value result;
        result["name"] = value.name;
        result["defaultValue"] = value.defaultValue;
        result["showBaseline"] = value.showBaseline;
        result["baselineValue"] = value.baselineValue;
        switch (value.fillMode) {
            case dto::FillMode::No: result["fillMode"] = "no"; break;
            case dto::FillMode::Top: result["fillMode"] = "top"; break;
            case dto::FillMode::Bottom: result["fillMode"] = "bottom"; break;
            case dto::FillMode::Baseline: result["fillMode"] = "baseline"; break;
        }
        result["fallbackOnDefaultValue"] = value.fallbackOnDefaultValue;
        result["displayValueMappingExpression"] = toJson(value.displayValueMappingExpression);
        result["displayValueInverseMappingExpression"] = toJson(value.displayValueInverseMappingExpression);
        result["displayValuePrefix"] = value.displayValuePrefix;
        result["displayValueSuffix"] = value.displayValueSuffix;
        result["displayValueDecimalPlaces"] = value.displayValueDecimalPlaces;
        return result;
    }

    Json::Value toJson(const dto::ArchitectureMetadata &value) {
        Json::Value result;
        result["id"] = value.id;
        result["name"] = value.name;
        result["parameters"] = toJson(value.parameters);
        result["defaultArchExtra"] = value.defaultArchExtra;
        return result;
    }

    Json::Value toJson(const dto::GroupMetadata &value) {
        Json::Value result;
        result["id"] = value.id;
        result["arch"] = value.arch;
        result["languages"] = toJson(value.languages);
        result["durationMode"] = value.durationMode == dto::DurationMode::Full ? "full" : "skip";
        result["parameterPipeline"] = Json::Value(Json::objectValue);
        const auto visitor = Overload{
            [](const dto::DirectParameter &) {
                Json::Value result;
                result["type"] = "direct";
                return result;
            },
            [](const dto::IndirectParameter &parameter) {
                Json::Value result;
                result["type"] = "indirect";
                result["dependsOn"] = toJson(parameter.dependsOn);
                result["retakeMode"] = parameter.retakeMode == dto::ParameterRetakeMode::Range ? "range" : "full";
                return result;
            }};
        for (const auto &[key, parameter] : value.parameterPipeline) {
            result["parameterPipeline"][key] = std::visit(visitor, parameter);
        }
        result["audioDependencies"] = toJson(value.audioDependencies);
        result["mixable"] = value.mixable;
        result["archSpecificInfo"] = value.archSpecificInfo;
        return result;
    }

    Json::Value toJson(const dto::GroupLanguageInfo &value) {
        Json::Value result;
        result["name"] = value.name;
        result["defaultLyric"] = value.defaultLyric;
        result["pronunciationMode"] = value.pronunciationMode == dto::PronunciationMode::Full ? "full" : "skip";
        switch (value.phonemeMode) {
            case dto::PhonemeMode::Full: result["phonemeMode"] = "full"; break;
            case dto::PhonemeMode::TokenOnly: result["phonemeMode"] = "token_only"; break;
            case dto::PhonemeMode::Skip: result["phonemeMode"] = "skip"; break;
        }
        return result;
    }

    Json::Value toJson(const dto::SingerInfo &value) {
        Json::Value result;
        result["id"] = value.id;
        result["name"] = value.name;
        result["arch"] = value.arch;
        result["group"] = value.group;
        result["defaultLanguage"] = value.defaultLanguage;
        result["archSpecificInfo"] = value.archSpecificInfo;
        result["defaultExtra"] = value.defaultExtra;
        return result;
    }

    Json::Value toJson(const dto::SingerAvatarResponse &value) {
        Json::Value result;
        result["avatarUrl"] = value.avatarUrl;
        return result;
    }

    Json::Value toJson(const dto::SingerBackgroundResponse &value) {
        Json::Value result;
        result["backgroundUrl"] = value.backgroundUrl;
        return result;
    }

    Json::Value toJson(const dto::SingerDemoAudio &value) {
        Json::Value result;
        result["name"] = value.name;
        result["audioUrl"] = value.audioUrl;
        return result;
    }

    Json::Value toJson(const dto::ExtractorInfo &value) {
        Json::Value result;
        result["id"] = value.id;
        result["name"] = value.name;
        result["preferredAudioSampleRate"] = value.preferredAudioSampleRate;
        if (value.acceptableFormats) {
            result["acceptableFormats"] = toJson(*value.acceptableFormats);
        }
        if (value.acceptableSchemes) {
            result["acceptableSchemes"] = toJson(*value.acceptableSchemes);
        }
        if (value.acceptableAudioSampleRates) {
            result["acceptableAudioSampleRates"] = toJson(*value.acceptableAudioSampleRates);
        }
        return result;
    }

    Json::Value toJson(const dto::SeparationTrackInfo &value) {
        Json::Value result;
        result["name"] = value.name;
        return result;
    }

    Json::Value toJson(const dto::SeparationExtractorInfo &value) {
        auto result = toJson(static_cast<const dto::ExtractorInfo &>(value));
        result["tracks"] = toJson(value.tracks);
        return result;
    }

    Json::Value toJson(const dto::ExtractorList &value) {
        Json::Value result;
        result["note"] = toJson(value.note);
        result["tempo"] = toJson(value.tempo);
        result["pitch"] = toJson(value.pitch);
        result["separation"] = toJson(value.separation);
        return result;
    }

    Json::Value toJson(const dto::EnvTagResponse &value) {
        Json::Value result;
        result["envTag"] = value.envTag;
        return result;
    }

    Json::Value toJson(const dto::Pronunciation &value) {
        Json::Value result;
        result["pronunciation"] = value.pronunciation;
        result["candidates"] = toJson(value.candidates);
        return result;
    }

    Json::Value toJson(const dto::Phoneme &value) {
        Json::Value result;
        result["token"] = value.token;
        result["onset"] = value.onset;
        return result;
    }

    Json::Value toJson(const dto::PhonemeNote &value) {
        Json::Value result;
        result["phonemes"] = toJson(value.phonemes);
        return result;
    }

    Json::Value toJson(const dto::DurationOutputPhoneme &value) {
        Json::Value result;
        result["start"] = value.start;
        return result;
    }

    Json::Value toJson(const dto::DurationOutputNote &value) {
        Json::Value result;
        result["phonemes"] = toJson(value.phonemes);
        return result;
    }

    Json::Value toJson(const dto::ParameterOutputParameter &value) {
        Json::Value result;
        result["values"] = toJson(value.values);
        result["sampleRate"] = value.sampleRate;
        return result;
    }

    Json::Value toJson(const dto::ParameterOutput &value) {
        Json::Value result;
        result["parameters"] = toJson(value.parameters);
        return result;
    }

    Json::Value toJson(const dto::AudioOutput &value) {
        Json::Value result;
        result["audioUrl"] = value.audioUrl;
        return result;
    }

    Json::Value toJson(const dto::NotePosition &value) {
        Json::Value result;
        result["gap"] = value.gap;
        result["duration"] = value.duration;
        return result;
    }

    Json::Value toJson(const dto::ExtractedNote &value) {
        Json::Value result;
        result["position"] = toJson(value.position);
        result["cent"] = value.cent;
        return result;
    }

    Json::Value toJson(const dto::PitchExtractionSegment &value) {
        Json::Value result;
        result["gap"] = toJson(value.gap);
        result["pitch"] = toJson(value.pitch);
        return result;
    }

    Json::Value toJson(const dto::PitchExtractionOutput &value) {
        Json::Value result;
        result["segments"] = toJson(value.segments);
        result["sampleRate"] = value.sampleRate;
        return result;
    }

    Json::Value toJson(const dto::TempoExtractionOutput &value) {
        Json::Value result;
        result["beats"] = toJson(value.beats);
        return result;
    }

    Json::Value toJson(const dto::ExtractedBeat &value) {
        Json::Value result;
        result["position"] = value.position;
        result["downbeat"] = value.downbeat;
        return result;
    }

    Json::Value toJson(const dto::SeparationTrack &value) {
        Json::Value result;
        result["name"] = value.name;
        result["audioUrl"] = value.audioUrl;
        return result;
    }

    Json::Value toJson(const dto::SeparationExtractionOutput &value) {
        Json::Value result;
        result["tracks"] = toJson(value.tracks);
        return result;
    }

}
