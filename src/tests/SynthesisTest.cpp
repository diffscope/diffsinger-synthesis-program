#include <chrono>
#include <future>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <dssparchdiffsinger/DiffSinger.h>
#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/SynthesisContext.h>
#include <dssparchdiffsinger/SynthesisInferences.h>
#include <dssparchdiffsinger/SynthesisScore.h>
#include <dssptests/TestLanguagePackage.h>

namespace dssp::tests {
    using namespace std::chrono_literals;

    TEST(SynthesisOptions, StageStepsDefaultIndependently) {
        Json::Value extra(Json::objectValue);
        for (const auto stage : {"pitch", "variance", "acoustic"}) {
            extra["steps"] = Json::Value(Json::objectValue);
            extra["steps"][stage] = 30;
            const auto options = arch_diffsinger::synthesisOptions(extra);
            ASSERT_TRUE(options);
            EXPECT_EQ(options->steps.pitch, extra["steps"].isMember("pitch") ? 30 : 20);
            EXPECT_EQ(options->steps.variance, extra["steps"].isMember("variance") ? 30 : 20);
            EXPECT_EQ(options->steps.acoustic, extra["steps"].isMember("acoustic") ? 30 : 20);
        }
        for (const bool includeSteps : {false, true}) {
            extra = Json::Value(Json::objectValue);
            if (includeSteps) extra["steps"] = Json::Value(Json::objectValue);
            const auto options = arch_diffsinger::synthesisOptions(extra);
            ASSERT_TRUE(options);
            EXPECT_EQ(options->steps.pitch, 20);
            EXPECT_EQ(options->steps.variance, 20);
            EXPECT_EQ(options->steps.acoustic, 20);
            EXPECT_FLOAT_EQ(options->depth, 1);
        }
    }

    TEST(SynthesisOptions, InvalidStepsReturnTypedInputErrors) {
        Json::Value extra(Json::objectValue);
        for (const auto &invalid : {Json::Value(), Json::Value(20), Json::Value("20"), Json::Value(true), Json::Value(Json::arrayValue)}) {
            extra["steps"] = invalid;
            const auto options = arch_diffsinger::synthesisOptions(extra);
            ASSERT_FALSE(options);
            EXPECT_NE(options.error().get_if<arch::InvalidInputError>(), nullptr);
        }
        for (const auto stage : {"pitch", "variance", "acoustic"}) {
            for (const auto &invalid : {Json::Value(), Json::Value(0), Json::Value(-1), Json::Value(1.5), Json::Value("20"), Json::Value(true), Json::Value(Json::arrayValue), Json::Value(Json::objectValue), Json::Value(std::numeric_limits<Json::UInt64>::max())}) {
                extra["steps"] = Json::Value(Json::objectValue);
                extra["steps"][stage] = invalid;
                const auto options = arch_diffsinger::synthesisOptions(extra);
                ASSERT_FALSE(options) << stage;
                EXPECT_NE(options.error().get_if<arch::InvalidInputError>(), nullptr);
            }
        }
    }

    TEST(SynthesisOptions, StageStepsAcceptSigned64BitLimits) {
        Json::Value extra(Json::objectValue);
        extra["steps"]["pitch"] = Json::Int64{1};
        extra["steps"]["variance"] = std::numeric_limits<Json::Int64>::max();
        extra["steps"]["acoustic"] = Json::Int64{30};
        const auto options = arch_diffsinger::synthesisOptions(extra);
        ASSERT_TRUE(options);
        EXPECT_EQ(options->steps.pitch, 1);
        EXPECT_EQ(options->steps.variance, std::numeric_limits<Json::Int64>::max());
        EXPECT_EQ(options->steps.acoustic, 30);
    }

    class SynthesisTest : public ::testing::Test {
    protected:
        void SetUp() override {
            LanguagePackageOptions package;
            package.singer = true;
            writeLanguagePackage(m_files, "package", package);
            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
            options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
            const auto host = inference::Environment::create(options);
            ASSERT_TRUE(host);
            m_environment = *host;
            ASSERT_TRUE(m_environment->problems().empty());
            const auto resources = media::Resources::create(std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "media" / "magic.mgc");
            ASSERT_TRUE(resources);
            const auto created = arch_diffsinger::create({m_environment, *resources, std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization"});
            ASSERT_TRUE(created);
            m_implementation = *created;
            const auto singers = m_implementation->singers({});
            ASSERT_TRUE(singers);
            ASSERT_EQ(singers->size(), 1U);
            m_context = {Json::Value(Json::objectValue), {{singers->front().id, Json::Value(Json::objectValue)}}};
        }

        void TearDown() override { if (m_environment) m_environment->shutdown(); }

        template <class Output, class Function>
        arch::SynthesisResult<Output> await(Function submit) {
            const auto promise = std::make_shared<std::promise<arch::SynthesisResult<Output>>>();
            auto future = promise->get_future();
            submit([promise](auto result) { promise->set_value(std::move(result)); });
            if (future.wait_for(5s) != std::future_status::ready) {
                ADD_FAILURE() << "Synthesis callback did not finish";
                return fail<arch::SynthesisErrors>(arch::OperationError("Test callback timed out"));
            }
            return future.get();
        }

        arch::AudioInput audioInput() const {
            arch::AudioInput input;
            input.context = m_context;
            input.piece.pieceDuration = 0.1;
            input.piece.mixSampleRate = 100;
            for (const auto id : {"pitch", "energy", "gender", "velocity", "tone_shift"}) {
                input.parameters.emplace(id, arch::AudioParameter{100, std::nullopt});
            }
            return input;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        arch::ArchitecturePtr m_implementation;
        arch::MultiSingerContext m_context;
    };

    TEST_F(SynthesisTest, AbsentRetakesAndZeroRetakesNeverLoadInferenceModels) {
        arch::ParameterInput input;
        input.context = m_context;
        input.piece.pieceDuration = 1;
        input.parameters.emplace("energy", arch::Parameter{{}, 100, std::nullopt});
        const auto conditionOnly = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter(input, {}, std::move(callback)); });
        ASSERT_TRUE(conditionOnly);
        EXPECT_TRUE(conditionOnly->parameters.empty());
        input.parameters.at("energy").retake = arch::ParameterRetake{0, 0};
        const auto noChange = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter(input, {}, std::move(callback)); });
        ASSERT_TRUE(noChange);
        ASSERT_EQ(noChange->parameters.size(), 1U);
        EXPECT_EQ(noChange->parameters.at("energy").values, std::vector<double>{1});
        for (const auto stage : {arch_diffsinger::SynthesisStage::Duration, arch_diffsinger::SynthesisStage::Pitch, arch_diffsinger::SynthesisStage::Variance, arch_diffsinger::SynthesisStage::Acoustic, arch_diffsinger::SynthesisStage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(SynthesisTest, SilentAndZeroLengthPiecesReturnWavWithoutLoadingModels) {
        auto input = audioInput();
        const auto silent = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_TRUE(silent) << (silent ? "" : silent.error().asError().formatChain());
        EXPECT_TRUE(silent->audioUrl.starts_with("data:audio/wav;base64,"));
        input.piece.pieceDuration = 0;
        const auto empty = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_TRUE(empty);
        EXPECT_TRUE(empty->audioUrl.starts_with("data:audio/wav;base64,"));
        for (const auto stage : {arch_diffsinger::SynthesisStage::Duration, arch_diffsinger::SynthesisStage::Pitch, arch_diffsinger::SynthesisStage::Variance, arch_diffsinger::SynthesisStage::Acoustic, arch_diffsinger::SynthesisStage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(SynthesisTest, DependenciesAndNegotiationFailBeforeLoadingAndNeverIgnoreClientPreferences) {
        auto input = audioInput();
        input.parameters.erase("pitch");
        auto result = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<arch::InvalidInputError>(), nullptr);
        input = audioInput();
        input.media.acceptableSchemes = {"https"};
        result = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<arch::InvalidInputError>(), nullptr);
        input.media.acceptableSchemes = {"data"};
        input.media.acceptableFormats = {"audio/flac"};
        result = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<arch::InvalidInputError>(), nullptr);
    }

    TEST_F(SynthesisTest, CancelledRequestsFinishBeforeLoadingOrPreparingSynthesis) {
        std::stop_source stop;
        stop.request_stop();
        const auto input = audioInput();
        const auto audio = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, stop.get_token(), std::move(callback)); });
        ASSERT_FALSE(audio);
        EXPECT_NE(audio.error().get_if<arch::CancelledError>(), nullptr);
        const auto duration = await<arch::DurationOutput>([&](auto callback) { m_implementation->duration({m_context, {}}, stop.get_token(), std::move(callback)); });
        ASSERT_FALSE(duration);
        EXPECT_NE(duration.error().get_if<arch::CancelledError>(), nullptr);
        const auto parameter = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter({m_context, {}}, stop.get_token(), std::move(callback)); });
        ASSERT_FALSE(parameter);
        EXPECT_NE(parameter.error().get_if<arch::CancelledError>(), nullptr);
        for (const auto stage : {arch_diffsinger::SynthesisStage::Duration, arch_diffsinger::SynthesisStage::Pitch, arch_diffsinger::SynthesisStage::Variance, arch_diffsinger::SynthesisStage::Acoustic, arch_diffsinger::SynthesisStage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(SynthesisTest, UnsupportedTokensAndDirectParameterRetakesFailBeforeLoading) {
        auto input = audioInput();
        arch::ParameterNote note;
        note.position.duration = 0.1;
        note.pronunciation = "unknown";
        note.language = "eng";
        note.phonemes.push_back({{"unknown", true, "eng"}, 0});
        input.piece.notes.push_back(note);
        const auto audio = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(audio);
        EXPECT_NE(audio.error().get_if<arch::InvalidInputError>(), nullptr);
        arch::ParameterInput parameters;
        parameters.context = m_context;
        parameters.piece.pieceDuration = 1;
        parameters.parameters.emplace("gender", arch::Parameter{{0.5}, 100, arch::ParameterRetake{0, 100}});
        const auto parameter = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter(parameters, {}, std::move(callback)); });
        ASSERT_FALSE(parameter);
        EXPECT_NE(parameter.error().get_if<arch::InvalidInputError>(), nullptr);
        for (const auto stage : {arch_diffsinger::SynthesisStage::Duration, arch_diffsinger::SynthesisStage::Pitch, arch_diffsinger::SynthesisStage::Variance, arch_diffsinger::SynthesisStage::Acoustic, arch_diffsinger::SynthesisStage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(SynthesisTest, UnsupportedDurationIsAnInputErrorAndRealModelFailuresCarrySingerAndStageContext) {
        const auto duration = await<arch::DurationOutput>([&](auto callback) { m_implementation->duration({m_context, {}}, {}, std::move(callback)); });
        ASSERT_FALSE(duration);
        EXPECT_NE(duration.error().get_if<arch::InvalidInputError>(), nullptr);
        auto input = audioInput();
        arch::ParameterNote note;
        note.position.duration = 0.1;
        note.cent = 6900;
        note.pronunciation = "a";
        note.language = "eng";
        note.phonemes.push_back({{"a", true, "eng"}, 0});
        input.piece.notes.push_back(note);
        const auto result = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(result);
        const auto *error = result.error().get_if<arch::OperationError>();
        ASSERT_NE(error, nullptr);
        EXPECT_NE(error->message().find("acoustic"), std::string::npos);
        EXPECT_NE(error->message().find(m_context.singers[0].id), std::string::npos);
        EXPECT_NE(error->cause(), nullptr);
    }

    TEST_F(SynthesisTest, MappingPreservesSourceOrderAndMergesOnlyAfterStageSpecificMapping) {
        const auto *architecture = dynamic_cast<const arch_diffsinger::DiffSinger *>(m_implementation.get());
        ASSERT_NE(architecture, nullptr);
        const auto *prototype = architecture->catalog().find(m_context.singers[0].id);
        ASSERT_NE(prototype, nullptr);
        auto first = *prototype;
        auto second = *prototype;
        for (auto *record : {&first, &second}) {
            auto &stage = record->stages.at("acoustic");
            stage.usesSpeakers = true;
            stage.speakers = {"model-a", "model-b"};
            stage.embeddedSpeakers = {"model-a", "model-b"};
        }

        first.stages.at("acoustic").speakerMapping = {{"main", "model-a"}};
        second.stages.at("acoustic").speakerMapping = {{"main", "model-b"}};
        arch_diffsinger::SynthesisContext resolved;
        resolved.sources = {{&first, "main"}, {&second, "main"}};
        arch::Piece<arch::ParameterNote> piece;
        piece.pieceDuration = 1;
        arch::ParameterNote note;
        note.position.duration = 1;
        note.pronunciation = "a";
        note.phonemes.push_back({{"a", true, "eng"}, 0.5});
        piece.notes.push_back(note);
        const auto score = arch_diffsinger::synthesisScore(first, piece);
        ASSERT_TRUE(score);
        const std::vector<std::vector<double>> mix{{0.2}, {0.8}};
        const auto separate = arch_diffsinger::stageScore(*score, resolved, "acoustic", mix, 1);
        ASSERT_TRUE(separate);
        ASSERT_EQ(separate->speakers.size(), 2U);
        EXPECT_EQ(separate->speakers[0].proportions, (std::vector<double>{0.2, 0.8}));
        EXPECT_NEAR(separate->words[0].phones[0].speakers[0].proportion, 0.5, 1e-12);
        EXPECT_NEAR(separate->words[0].phones[0].speakers[1].proportion, 0.5, 1e-12);
        second.stages.at("acoustic").speakerMapping["main"] = "model-a";
        const auto merged = arch_diffsinger::stageScore(*score, resolved, "acoustic", mix, 1);
        ASSERT_TRUE(merged);
        ASSERT_EQ(merged->speakers.size(), 1U);
        EXPECT_EQ(merged->speakers[0].proportions, (std::vector<double>{1, 1}));
        EXPECT_EQ(merged->words[0].phones[0].speakers.size(), 1U);
        EXPECT_DOUBLE_EQ(merged->words[0].phones[0].speakers[0].proportion, 1);
    }

    TEST_F(SynthesisTest, EnvironmentTagsNormalizeDefaultsIncludeEffectiveOptionsAndDoNotLoadModels) {
        const auto tag = m_implementation->envTag(m_context);
        ASSERT_TRUE(tag);
        EXPECT_TRUE(tag->starts_with("ds1."));
        auto emptySteps = m_context;
        emptySteps.archExtra["steps"] = Json::Value(Json::objectValue);
        const auto emptyStepsTag = m_implementation->envTag(emptySteps);
        ASSERT_TRUE(emptyStepsTag);
        EXPECT_EQ(*tag, *emptyStepsTag);
        auto explicitDefaults = m_context;
        for (const auto stage : {"pitch", "variance", "acoustic"}) explicitDefaults.archExtra["steps"][stage] = 20;
        explicitDefaults.archExtra["depth"] = 1;
        explicitDefaults.singers[0].extra["speaker"] = "";
        const auto same = m_implementation->envTag(explicitDefaults);
        ASSERT_TRUE(same);
        EXPECT_EQ(*tag, *same);
        std::vector<std::string> changedTags;
        for (const auto stage : {"pitch", "variance", "acoustic"}) {
            auto changedContext = explicitDefaults;
            changedContext.archExtra["steps"][stage] = 21;
            const auto changed = m_implementation->envTag(changedContext);
            ASSERT_TRUE(changed);
            EXPECT_NE(*tag, *changed);
            for (const auto &previous : changedTags) EXPECT_NE(previous, *changed);
            changedTags.push_back(*changed);
        }
        auto unknown = m_context;
        unknown.singers[0].id = "missing";
        EXPECT_FALSE(m_implementation->envTag(unknown));
        // Current contract deliberately trusts package ID/version until the
        // package manager supplies hashes; no model file content is scanned.
        m_files.write("package/missing-acoustic.onnx", "changed bytes, same package version");
        const auto unchanged = m_implementation->envTag(m_context);
        ASSERT_TRUE(unchanged);
        EXPECT_EQ(*tag, *unchanged);
        for (const auto stage : {arch_diffsinger::SynthesisStage::Duration, arch_diffsinger::SynthesisStage::Pitch, arch_diffsinger::SynthesisStage::Variance, arch_diffsinger::SynthesisStage::Acoustic, arch_diffsinger::SynthesisStage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }
}
