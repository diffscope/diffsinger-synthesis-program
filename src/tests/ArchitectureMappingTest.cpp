#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dsspapiv1/ArchitectureMapping.h>
#include <dsspapiv1/ResponseCodec.h>

namespace dssp::tests {

    TEST(ArchitectureMapping, ParameterInputRetainsSourceOrderAndIndependentSamplingGrids) {
        api_v1::dto::ParameterRequest request;
        request.context.arch = "diffsinger";
        request.context.archExtra["steps"]["pitch"] = 12;
        request.context.archExtra["steps"]["variance"] = 24;
        request.context.archExtra["steps"]["acoustic"] = 36;
        api_v1::dto::Singer lead{"lead", Json::Value{}};
        lead.extra["speaker"] = "soft";
        request.context.singers = {std::move(lead), {"backing", Json::Value{}}};
        request.input.pieceDuration = 1250.5;
        request.input.mix = {{0.25, 0.75}, {0.8, 0.2}};
        request.input.mixSampleRate = 37;
        api_v1::dto::ParameterNote first;
        first.position = {4.5, 125.25};
        first.cent = 12000;
        first.pronunciation = "la";
        first.language = "eng";
        first.phonemes = {{{"l", true, "eng"}, -15.75}, {{"a", false, "eng"}, 0}};
        api_v1::dto::ParameterNote slur;
        slur.kind = api_v1::dto::NoteKind::Slur;
        request.input.notes = {std::move(first), std::move(slur)};
        request.input.parameters.emplace("pitch", api_v1::dto::Parameter{{0.3, 0.7}, 83, api_v1::dto::ParameterRetake{41, 9}});
        request.input.parameters.emplace("energy", api_v1::dto::Parameter{{0.5}, 19, std::nullopt});

        const auto input = api_v1::toArchitecture(std::move(request));
        ASSERT_EQ(input.context.singers.size(), 2U);
        EXPECT_EQ(input.context.singers[0].id, "lead");
        EXPECT_EQ(input.context.singers[1].id, "backing");
        EXPECT_EQ(input.context.singers[0].extra["speaker"].asString(), "soft");
        EXPECT_TRUE(input.context.singers[1].extra.isNull());
        EXPECT_EQ(input.context.archExtra["steps"]["pitch"].asInt(), 12);
        EXPECT_EQ(input.context.archExtra["steps"]["variance"].asInt(), 24);
        EXPECT_EQ(input.context.archExtra["steps"]["acoustic"].asInt(), 36);
        EXPECT_DOUBLE_EQ(input.piece.pieceDuration, 1250.5);
        EXPECT_EQ(input.piece.mix, (std::vector<std::vector<double>>{{0.25, 0.75}, {0.8, 0.2}}));
        EXPECT_DOUBLE_EQ(input.piece.mixSampleRate, 37);
        ASSERT_EQ(input.piece.notes.size(), 2U);
        const auto &note = input.piece.notes[0];
        EXPECT_DOUBLE_EQ(note.position.gap, 4.5);
        EXPECT_DOUBLE_EQ(note.position.duration, 125.25);
        EXPECT_EQ(note.cent, 12000U);
        EXPECT_EQ(note.pronunciation, "la");
        EXPECT_EQ(note.language, "eng");
        ASSERT_EQ(note.phonemes.size(), 2U);
        EXPECT_EQ(note.phonemes[0].token, "l");
        EXPECT_TRUE(note.phonemes[0].onset);
        EXPECT_EQ(note.phonemes[0].language, "eng");
        EXPECT_DOUBLE_EQ(note.phonemes[0].start, -15.75);
        EXPECT_EQ(input.piece.notes[1].kind, arch::NoteKind::Slur);
        const auto pitch = input.parameters.find("pitch");
        ASSERT_NE(pitch, input.parameters.end());
        EXPECT_EQ(pitch->second.values, (std::vector<double>{0.3, 0.7}));
        EXPECT_DOUBLE_EQ(pitch->second.sampleRate, 83);
        ASSERT_TRUE(pitch->second.retake);
        EXPECT_EQ(pitch->second.retake->position, 41U);
        EXPECT_EQ(pitch->second.retake->length, 9U);
        const auto energy = input.parameters.find("energy");
        ASSERT_NE(energy, input.parameters.end());
        EXPECT_DOUBLE_EQ(energy->second.sampleRate, 19);
        EXPECT_FALSE(energy->second.retake);
    }

    TEST(ArchitectureMapping, PhonemeInputRetainsOptionalAndNegativeSliceEndpoints) {
        api_v1::dto::PhonemeRequest request;
        request.context.arch = "diffsinger";
        request.context.singer.id = "singer";
        request.input.notes = {{"la", "eng", -3, std::nullopt}, {"", "jpn", std::nullopt, -1}};
        const auto input = api_v1::toArchitecture(std::move(request));
        EXPECT_EQ(input.context.singer.id, "singer");
        EXPECT_TRUE(input.context.archExtra.isNull());
        ASSERT_EQ(input.notes.size(), 2U);
        EXPECT_EQ(input.notes[0].pronunciation, "la");
        EXPECT_EQ(input.notes[0].syllableSliceStart, -3);
        EXPECT_FALSE(input.notes[0].syllableSliceEnd);
        EXPECT_TRUE(input.notes[1].pronunciation.empty());
        EXPECT_EQ(input.notes[1].language, "jpn");
        EXPECT_FALSE(input.notes[1].syllableSliceStart);
        EXPECT_EQ(input.notes[1].syllableSliceEnd, -1);
    }

    TEST(ArchitectureMapping, AudioInputRetainsMediaOrderAndDistinguishesMissingFromEmptyValues) {
        api_v1::dto::AudioRequest request;
        request.context.arch = "diffsinger";
        request.input.parameters.emplace("missing", api_v1::dto::AudioParameter{43, std::nullopt});
        request.input.parameters.emplace("empty", api_v1::dto::AudioParameter{71, std::vector<double>{}});
        request.input.parameters.emplace("pitch", api_v1::dto::AudioParameter{83, std::vector<double>{0.2, 0.8}});
        request.acceptableFormats = std::vector<std::string>{"Audio/FLAC", "audio/wav"};
        request.acceptableSchemes = std::vector<std::string>{"HTTPS", "data"};
        const auto input = api_v1::toArchitecture(std::move(request));
        EXPECT_EQ(input.media.acceptableFormats, (std::vector<std::string>{"Audio/FLAC", "audio/wav"}));
        EXPECT_EQ(input.media.acceptableSchemes, (std::vector<std::string>{"HTTPS", "data"}));
        const auto missing = input.parameters.find("missing");
        ASSERT_NE(missing, input.parameters.end());
        EXPECT_DOUBLE_EQ(missing->second.sampleRate, 43);
        EXPECT_FALSE(missing->second.values);
        const auto empty = input.parameters.find("empty");
        ASSERT_NE(empty, input.parameters.end());
        ASSERT_TRUE(empty->second.values);
        EXPECT_TRUE(empty->second.values->empty());
        const auto pitch = input.parameters.find("pitch");
        ASSERT_NE(pitch, input.parameters.end());
        ASSERT_TRUE(pitch->second.values);
        EXPECT_EQ(*pitch->second.values, (std::vector<double>{0.2, 0.8}));
        const auto unrestricted = api_v1::toArchitecture(api_v1::dto::AudioRequest{});
        EXPECT_TRUE(unrestricted.media.acceptableFormats.empty());
        EXPECT_TRUE(unrestricted.media.acceptableSchemes.empty());
    }

    TEST(ArchitectureMapping, CatalogOutputRetainsRecursiveExpressionsAndPipelineKinds) {
        arch::ParameterPresentation parameter;
        parameter.fillMode = arch::FillMode::Baseline;
        parameter.displayValueMappingExpression.value = arch::AST::Operation{"*", {arch::AST{12800.0}, arch::AST{arch::AST::Operation{"x", {}}}}};
        const auto metadata = api_v1::toJson(api_v1::toDto(arch::ArchitectureMetadata{"diffsinger", "DiffSinger", {{"pitch", std::move(parameter)}}, {}}));
        EXPECT_EQ(metadata["id"].asString(), "diffsinger");
        const auto &presentation = metadata["parameters"]["pitch"];
        EXPECT_EQ(presentation["fillMode"].asString(), "baseline");
        const auto &expression = presentation["displayValueMappingExpression"];
        EXPECT_EQ(expression["operator"].asString(), "*");
        EXPECT_DOUBLE_EQ(expression["operands"][0].asDouble(), 12800.0);
        EXPECT_EQ(expression["operands"][1]["operator"].asString(), "x");

        arch::GroupMetadata group;
        group.id = "group";
        group.arch = "diffsinger";
        group.languages.emplace("eng", arch::GroupLanguageInfo{"English", "lyric", arch::PronunciationMode::Full, arch::PhonemeMode::TokenOnly});
        group.languages.emplace("cmn", arch::GroupLanguageInfo{"Mandarin", {}, arch::PronunciationMode::Full, arch::PhonemeMode::Full});
        group.languages.emplace("deu", arch::GroupLanguageInfo{"German", {}, arch::PronunciationMode::Skip, arch::PhonemeMode::Skip});
        group.durationMode = arch::DurationMode::Full;
        group.parameterPipeline.emplace("pitch", arch::DirectParameter{});
        group.parameterPipeline.emplace("voicing", arch::DirectParameter{});
        group.parameterPipeline.emplace("energy", arch::IndirectParameter{{"pitch", "voicing"}, arch::ParameterRetakeMode::Range});
        group.parameterPipeline.emplace("breathiness", arch::IndirectParameter{{"pitch"}, arch::ParameterRetakeMode::Full});
        group.audioDependencies = {"pitch", "energy"};
        group.mixable = false;
        const auto body = api_v1::toJson(api_v1::toDto(std::move(group)));
        EXPECT_FALSE(body.isMember("pronunciationMode"));
        EXPECT_FALSE(body.isMember("phonemeMode"));
        EXPECT_EQ(body["languages"]["eng"]["name"].asString(), "English");
        EXPECT_EQ(body["languages"]["eng"]["defaultLyric"].asString(), "lyric");
        EXPECT_EQ(body["languages"]["eng"]["pronunciationMode"].asString(), "full");
        EXPECT_EQ(body["languages"]["eng"]["phonemeMode"].asString(), "token_only");
        EXPECT_EQ(body["languages"]["cmn"]["phonemeMode"].asString(), "full");
        EXPECT_EQ(body["languages"]["deu"]["pronunciationMode"].asString(), "skip");
        EXPECT_EQ(body["languages"]["deu"]["phonemeMode"].asString(), "skip");
        EXPECT_EQ(body["durationMode"].asString(), "full");
        EXPECT_EQ(body["parameterPipeline"]["pitch"]["type"].asString(), "direct");
        EXPECT_EQ(body["parameterPipeline"]["energy"]["type"].asString(), "indirect");
        EXPECT_EQ(body["parameterPipeline"]["energy"]["dependsOn"][0].asString(), "pitch");
        EXPECT_EQ(body["parameterPipeline"]["energy"]["dependsOn"][1].asString(), "voicing");
        EXPECT_EQ(body["parameterPipeline"]["energy"]["retakeMode"].asString(), "range");
        EXPECT_EQ(body["parameterPipeline"]["breathiness"]["retakeMode"].asString(), "full");
        EXPECT_FALSE(body["parameterPipeline"]["pitch"].isMember("retakeMode"));
        EXPECT_EQ(body["audioDependencies"][1].asString(), "energy");
        EXPECT_FALSE(body["mixable"].asBool());
    }

    TEST(ArchitectureMapping, CatalogExtensionsRetainEveryJsonTypeIncludingNull) {
        Json::Value array(Json::arrayValue), object(Json::objectValue);
        array.append("item");
        object["nested"] = array;
        const std::vector<Json::Value> values{Json::Value{}, Json::Value(Json::objectValue), array, object, Json::Value("text"), Json::Value(true), Json::Value(7), Json::Value(0.25)};
        for (std::size_t i = 0; i < values.size(); ++i) {
            SCOPED_TRACE(i);
            arch::GroupMetadata group;
            group.archSpecificInfo = values[i];
            const auto body = api_v1::toJson(api_v1::toDto(std::move(group)));
            EXPECT_TRUE(body.isMember("archSpecificInfo"));
            EXPECT_FALSE(body.isMember("defaultArchExtra"));
            EXPECT_EQ(body["archSpecificInfo"], values[i]);
            arch::ArchitectureMetadata architecture;
            architecture.defaultArchExtra = values[i];
            const auto metadata = api_v1::toJson(api_v1::toDto(std::move(architecture)));
            EXPECT_TRUE(metadata.isMember("defaultArchExtra"));
            EXPECT_EQ(metadata["defaultArchExtra"], values[i]);
        }
    }

    TEST(ArchitectureMapping, SingerOutputRetainsArbitraryExtensionsAndDefaultLanguageWithoutSharedCapabilities) {
        arch::SingerInfo singer;
        singer.id = "singer";
        singer.arch = "diffsinger";
        singer.group = "group";
        singer.defaultLanguage = "eng";
        singer.archSpecificInfo = Json::Value(Json::arrayValue);
        singer.archSpecificInfo.append("speaker-A");
        singer.defaultExtra = 7;
        const auto body = api_v1::toJson(api_v1::toDto(std::move(singer)));
        EXPECT_EQ(body["arch"].asString(), "diffsinger");
        EXPECT_EQ(body["group"].asString(), "group");
        EXPECT_FALSE(body.isMember("languages"));
        EXPECT_EQ(body["defaultLanguage"].asString(), "eng");
        EXPECT_TRUE(body["archSpecificInfo"].isArray());
        EXPECT_EQ(body["archSpecificInfo"][0].asString(), "speaker-A");
        EXPECT_EQ(body["defaultExtra"].asInt(), 7);
    }

    TEST(ArchitectureMapping, StageOutputRetainsRelativeStartsAndParameterValues) {
        arch::DurationOutput durationOutput;
        durationOutput.notes.push_back(arch::DurationOutputNote{{{-15.75}, {0}}});
        const auto duration = api_v1::toJson(api_v1::dto::DurationResponse{api_v1::toDto(std::move(durationOutput))});
        ASSERT_EQ(duration["output"]["notes"].size(), 1U);
        EXPECT_DOUBLE_EQ(duration["output"]["notes"][0]["phonemes"][0]["start"].asDouble(), -15.75);
        arch::ParameterOutput parameterOutput;
        parameterOutput.parameters.emplace("pitch", arch::ParameterOutputParameter{{0.2, 0.8}, 83});
        const auto parameters = api_v1::toJson(api_v1::dto::ParameterResponse{api_v1::toDto(std::move(parameterOutput))});
        EXPECT_DOUBLE_EQ(parameters["output"]["parameters"]["pitch"]["sampleRate"].asDouble(), 83);
        EXPECT_DOUBLE_EQ(parameters["output"]["parameters"]["pitch"]["values"][1].asDouble(), 0.8);
        EXPECT_FALSE(parameters.isMember("state"));
    }

}
