#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#include <gtest/gtest.h>
#include <dsinfer/Api/Singers/DiffSinger/1/DiffSingerApiL1.h>

#include <dssparchdiffsinger/DiffSinger.h>
#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/SynthesisInferences.h>
#include <dssparchdiffsinger/SynthesisParameters.h>
#include <dsspresource/TaskProgress.h>
#include <dssptests/TestLanguagePackage.h>

namespace dssp::tests {
    using namespace std::chrono_literals;
    namespace Du = ds::Api::Duration::L1;
    namespace Pi = ds::Api::Pitch::L1;
    namespace Va = ds::Api::Variance::L1;
    namespace Ac = ds::Api::Acoustic::L1;
    namespace Vo = ds::Api::Vocoder::L1;
    namespace Common = ds::Api::Common::L1;
    using Stage = arch_diffsinger::SynthesisStage;

    namespace {
        template <class Executive, class Init, class Input, class Result>
        class TestExecutive final : public Executive {
        public:
            using Hook = std::function<std::unique_ptr<Result>(const Input &)>;

            TestExecutive(srt::InferenceSpec &spec, Hook hook) : Executive(spec), m_hook(std::move(hook)) {}

            srt::Expected<void> initialize(const Init &) override { return {}; }

            srt::Expected<std::unique_ptr<Result>> start(const Input &input) override {
                m_state = srt::ITask::Running;
                auto result = m_hook(input);
                m_state = srt::ITask::Succeeded;
                return result;
            }

            srt::Expected<void> startAsync(std::shared_ptr<const Input> input, typename Executive::AsyncCallback callback) override {
                callback(start(*input));
                return {};
            }

            srt::ITask::State state() const noexcept override { return m_state; }

            srt::Expected<void> stop() override {
                m_state = srt::ITask::Canceled;
                return {};
            }

            srt::Expected<void> waitForFinished() override { return {}; }

        private:
            Hook m_hook;
            std::atomic<srt::ITask::State> m_state = srt::ITask::Idle;
        };

        using DurationExecutive = TestExecutive<Du::DurationExecutive, Du::DurationInitArgs, Du::DurationStartInput, Du::DurationResult>;
        using PitchExecutive = TestExecutive<Pi::PitchExecutive, Pi::PitchInitArgs, Pi::PitchStartInput, Pi::PitchResult>;
        using VarianceExecutive = TestExecutive<Va::VarianceExecutive, Va::VarianceInitArgs, Va::VarianceStartInput, Va::VarianceResult>;
        using AcousticExecutive = TestExecutive<Ac::AcousticExecutive, Ac::AcousticInitArgs, Ac::AcousticStartInput, Ac::AcousticResult>;
        using VocoderExecutive = TestExecutive<Vo::VocoderExecutive, Vo::VocoderInitArgs, Vo::VocoderStartInput, Vo::VocoderResult>;

        const Common::InputParameterInfo *curve(const std::vector<Common::InputParameterInfo> &parameters, std::string_view tag) {
            const auto found = std::ranges::find_if(parameters, [&](const auto &value) { return value.tag.name() == tag; });
            return found == parameters.end() ? nullptr : &*found;
        }
    }

    class SynthesisPipelineTest : public ::testing::Test {
    protected:
        void SetUp() override {
            writeLanguagePackage(m_files, "package", {.singer = true, .synthesisStages = true});
            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
            options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
            // Each stage owns a cache. No ONNX driver or model is needed;
            // only typed contract doubles execute.
            for (const auto stage : {Stage::Duration, Stage::Pitch, Stage::Variance, Stage::Acoustic, Stage::Vocoder}) {
                options.cacheGroups.emplace(arch_diffsinger::synthesisCacheGroup(stage), resource::CacheOptions{1, 0ms, 60s});
            }
            const auto created = inference::Environment::create(options);
            ASSERT_TRUE(created);
            m_environment = *created;
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
            const auto resources = media::Resources::create(std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "media" / "magic.mgc");
            ASSERT_TRUE(resources);
            const auto architecture = arch_diffsinger::create({m_environment, *resources, std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization"});
            ASSERT_TRUE(architecture);
            m_implementation = *architecture;
            const auto *typed = dynamic_cast<const arch_diffsinger::DiffSinger *>(m_implementation.get());
            ASSERT_NE(typed, nullptr);
            ASSERT_EQ(typed->catalog().singers().size(), 1U);
            m_singer = &typed->catalog().singers().begin()->second;
            m_context = {Json::Value(Json::objectValue), {{m_singer->info.id, Json::Value(Json::objectValue)}}};
        }

        void TearDown() override {
            m_implementation.reset();
            if (m_environment) m_environment->shutdown();
        }

        template <class Executive>
        void install(Stage stage, typename Executive::Hook hook) {
            const auto key = arch_diffsinger::synthesisInferenceKey(*m_singer, stage);
            ASSERT_TRUE(key);
            const auto &record = m_singer->stages.at(std::string(arch_diffsinger::stageName(stage)));
            auto *spec = dynamic_cast<srt::InferenceSpec *>(const_cast<srt::ContribSpec *>(record.spec));
            ASSERT_NE(spec, nullptr);
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            const auto handle = (*pool)->bind({*key, std::string(arch_diffsinger::stageName(stage)), {m_singer->package, spec->package()}, [spec, hook = std::move(hook)]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                return std::make_unique<Executive>(*spec, hook);
            }});
            ASSERT_TRUE(handle);
        }

        template <class Output, class Submit>
        arch::SynthesisResult<Output> await(Submit submit) {
            const auto promise = std::make_shared<std::promise<arch::SynthesisResult<Output>>>();
            auto future = promise->get_future();
            submit([promise](auto result) { promise->set_value(std::move(result)); });
            if (future.wait_for(5s) != std::future_status::ready) {
                ADD_FAILURE() << "Synthesis callback timed out";
                return fail<arch::SynthesisErrors>(arch::OperationError("Test callback timed out"));
            }
            return future.get();
        }

        arch::Piece<arch::ParameterNote> piece() const {
            arch::Piece<arch::ParameterNote> result;
            result.pieceDuration = 0.5;
            arch::ParameterNote note;
            note.position.duration = 0.5;
            note.cent = 6900;
            note.pronunciation = "a";
            note.language = "eng";
            note.phonemes.push_back({{"a", true, "eng"}, 0});
            result.notes.push_back(note);
            return result;
        }

        arch::ParameterInput parameterInput() const {
            arch::ParameterInput input;
            input.context = m_context;
            input.piece = piece();
            input.parameters.emplace("pitch", arch::Parameter{{69.0 / 128}, 100, arch::ParameterRetake{20, 20}});
            input.parameters.emplace("energy", arch::Parameter{{1}, 100, arch::ParameterRetake{0, 50}});
            input.parameters.emplace("tone_shift", arch::Parameter{{1}, 100, std::nullopt});
            return input;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        arch::ArchitecturePtr m_implementation;
        const arch_diffsinger::SingerRecord *m_singer = nullptr;
        arch::MultiSingerContext m_context;
    };

    TEST_F(SynthesisPipelineTest, DurationRestoresSubmittedPhonemeOrigins) {
        install<DurationExecutive>(Stage::Duration, [](const auto &input) {
            EXPECT_EQ(input.words.size(), 2U);
            auto result = std::make_unique<Du::DurationResult>();
            result->durations = {0.15, 0.05, 0.3};
            return result;
        });
        arch::DurationInput input;
        input.context = m_context;
        input.piece.pieceDuration = 0.5;
        arch::DurationNote note;
        note.position = {0.2, 0.3};
        note.cent = 6900;
        note.pronunciation = "b a";
        note.language = "eng";
        note.phonemes = {{"b", false, "eng"}, {"a", true, "eng"}};
        input.piece.notes.push_back(note);
        const auto output = await<arch::DurationOutput>([&](auto callback) { m_implementation->duration(input, {}, std::move(callback)); });
        ASSERT_TRUE(output) << (output ? "" : output.error().asError().formatChain());
        ASSERT_EQ(output->notes.size(), 1U);
        ASSERT_EQ(output->notes[0].phonemes.size(), 2U);
        EXPECT_NEAR(output->notes[0].phonemes[0].start, -0.05, 1e-12);
        EXPECT_NEAR(output->notes[0].phonemes[1].start, 0, 1e-12);
    }

    TEST_F(SynthesisPipelineTest, PitchRunsBeforeVarianceAndLockedConditionsStayIntact) {
        auto order = std::make_shared<std::vector<std::string>>();
        install<PitchExecutive>(Stage::Pitch, [order](const auto &input) {
            order->push_back("pitch");
            EXPECT_EQ(input.steps, 12);
            const auto *pitch = curve(input.parameters, "pitch");
            EXPECT_NE(pitch, nullptr);
            if (pitch && pitch->retake) {
                EXPECT_DOUBLE_EQ(pitch->retake->start, 0.2);
                EXPECT_DOUBLE_EQ(pitch->retake->end, 0.4);
            }

            auto result = std::make_unique<Pi::PitchResult>();
            result->interval = 0.01;
            result->pitch.assign(50, 80);
            return result;
        });
        install<VarianceExecutive>(Stage::Variance, [order](const auto &input) {
            order->push_back("variance");
            EXPECT_EQ(input.steps, 24);
            const auto *pitch = curve(input.parameters, "pitch");
            EXPECT_NE(pitch, nullptr);
            if (pitch && pitch->values.size() == 50) {
                EXPECT_DOUBLE_EQ(pitch->values[0], 81);
                EXPECT_DOUBLE_EQ(pitch->values[20], 92);
                EXPECT_DOUBLE_EQ(pitch->values[40], 81);
                EXPECT_TRUE(pitch->retake.has_value());
                if (pitch->retake) EXPECT_DOUBLE_EQ(pitch->retake->end, 0);
            }
            auto result = std::make_unique<Va::VarianceResult>();
            result->predictions.push_back({ds::ParamTag("energy"), std::vector<double>(50, -48), 0.01, std::nullopt});
            return result;
        });
        auto input = parameterInput();
        input.context.archExtra["steps"]["pitch"] = 12;
        input.context.archExtra["steps"]["variance"] = 24;
        input.context.archExtra["steps"]["acoustic"] = 36;
        std::vector<double> progress;
        input.progress = resource::TaskProgress([&](resource::TaskUpdate update) {
            if (update.state == resource::TaskUpdate::State::Progress && update.value > 0) progress.push_back(update.value);
        });
        const auto output = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter(input, {}, std::move(callback)); });
        ASSERT_TRUE(output) << (output ? "" : output.error().asError().formatChain());
        EXPECT_EQ(*order, (std::vector<std::string>{"pitch", "variance"}));
        EXPECT_EQ(progress, (std::vector<double>{0.5, 1}));
        ASSERT_EQ(output->parameters.size(), 2U);
        const auto &pitch = output->parameters.at("pitch");
        EXPECT_DOUBLE_EQ(pitch.sampleRate, 100);
        ASSERT_EQ(pitch.values.size(), 50U);
        EXPECT_DOUBLE_EQ(pitch.values[0], 69.0 / 128);
        EXPECT_DOUBLE_EQ(pitch.values[20], 80.0 / 128);
        EXPECT_DOUBLE_EQ(pitch.values[40], 69.0 / 128);
        EXPECT_NEAR(output->parameters.at("energy").values[0], 1 - std::pow(0.5, 1.25), 1e-12);
    }

    TEST_F(SynthesisPipelineTest, MalformedVarianceResultFailsTheWholeRequest) {
        install<PitchExecutive>(Stage::Pitch, [](const auto &) {
            auto result = std::make_unique<Pi::PitchResult>();
            result->interval = 0.01;
            result->pitch.assign(50, 80);
            return result;
        });
        install<VarianceExecutive>(Stage::Variance, [](const auto &) { return std::make_unique<Va::VarianceResult>(); });
        const auto input = parameterInput();
        const auto output = await<arch::ParameterOutput>([&](auto callback) { m_implementation->parameter(input, {}, std::move(callback)); });
        ASSERT_FALSE(output);
        EXPECT_NE(output.error().get_if<arch::OperationError>(), nullptr);
    }

    TEST_F(SynthesisPipelineTest, AcousticAndVocoderRunOnceAndOriginalPitchSurvivesToneShiftAndEviction) {
        const auto *configuration = dynamic_cast<const Ac::AcousticConfiguration *>(m_singer->stages.at("acoustic").spec->configuration());
        ASSERT_NE(configuration, nullptr);
        const double interval = static_cast<double>(configuration->hopSize) / configuration->sampleRate;
        const auto frames = static_cast<std::int64_t>(std::round(0.5 / interval));
        auto order = std::make_shared<std::vector<std::string>>();
        auto originalMel = std::make_shared<std::shared_ptr<ds::ITensor>>();
        install<AcousticExecutive>(Stage::Acoustic, [order, originalMel, frames, channels = configuration->melChannels](const auto &input) {
            order->push_back("acoustic");
            EXPECT_EQ(input.steps, 30);
            EXPECT_FLOAT_EQ(input.depth, 0.75f);
            const auto *pitch = curve(input.parameters, "pitch");
            EXPECT_NE(pitch, nullptr);
            if (pitch) EXPECT_DOUBLE_EQ(pitch->values.front(), 81);
            EXPECT_EQ(curve(input.parameters, "tone_shift"), nullptr);
            const auto mel = ds::Tensor::createFilled<float>({1, frames, channels}, 0.1f);
            const auto f0 = ds::Tensor::createFilled<float>({1, frames}, 880);
            EXPECT_TRUE(mel);
            EXPECT_TRUE(f0);
            auto result = std::make_unique<Ac::AcousticResult>();
            if (mel) result->mel = mel.get();
            if (f0) result->f0 = f0.get();
            *originalMel = result->mel;
            return result;
        });
        install<VocoderExecutive>(Stage::Vocoder, [order, originalMel, frames](const auto &input) {
            order->push_back("vocoder");
            EXPECT_NE(input.mel.get(), originalMel->get());
            EXPECT_EQ(input.mel->shape()[1], frames);
            const auto f0 = input.f0->template view<float>();
            EXPECT_EQ(f0.size(), static_cast<std::size_t>(frames));
            for (const auto value : f0) EXPECT_FLOAT_EQ(value, 440);
            auto result = std::make_unique<Vo::VocoderResult>();
            const float sample = 0.25f;
            result->audioData.resize(sizeof(sample));
            std::memcpy(result->audioData.data(), &sample, sizeof(sample));
            return result;
        });
        arch::AudioInput input;
        input.context = m_context;
        input.context.archExtra["steps"]["pitch"] = 12;
        input.context.archExtra["steps"]["variance"] = 24;
        input.context.archExtra["steps"]["acoustic"] = 30;
        input.context.archExtra["depth"] = 0.75;
        input.piece = piece();
        for (const auto id : {"pitch", "energy", "gender", "velocity", "tone_shift"}) input.parameters.emplace(id, arch::AudioParameter{100, std::nullopt});
        input.parameters.at("pitch").values = std::vector<double>{69.0 / 128};
        input.parameters.at("tone_shift").values = std::vector<double>{1};
        std::vector<double> progress;
        input.progress = resource::TaskProgress([&](resource::TaskUpdate update) {
            if (update.state == resource::TaskUpdate::State::Progress && update.value > 0) progress.push_back(update.value);
        });
        const auto output = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_TRUE(output) << (output ? "" : output.error().asError().formatChain());
        EXPECT_TRUE(output->audioUrl.starts_with("data:audio/wav;base64,"));
        EXPECT_EQ(*order, (std::vector<std::string>{"acoustic", "vocoder"}));
        EXPECT_EQ(progress, (std::vector<double>{0.9, 1}));
        for (const auto stage : {Stage::Acoustic, Stage::Vocoder}) {
            const auto pool = m_environment->inferences(arch_diffsinger::synthesisCacheGroup(stage));
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 1U);
        }

        const auto duration = m_environment->inferences("diffsinger-duration");
        ASSERT_TRUE(duration);
        EXPECT_EQ((*duration)->residentCount(), 0U);
    }

    TEST_F(SynthesisPipelineTest, ReservedBreathRunsAcousticAndMalformedTensorsNeverReachVocoder) {
        const auto acousticCalls = std::make_shared<std::atomic<unsigned>>(0);
        const auto vocoderCalls = std::make_shared<std::atomic<unsigned>>(0);
        install<AcousticExecutive>(Stage::Acoustic, [acousticCalls](const auto &input) {
            ++*acousticCalls;
            EXPECT_EQ(input.words.size(), 1U);
            if (!input.words.empty()) EXPECT_EQ(input.words.front().phones.front().token, "AP");
            // A successful native task with missing tensors is still an error.
            return std::make_unique<Ac::AcousticResult>();
        });
        install<VocoderExecutive>(Stage::Vocoder, [vocoderCalls](const auto &) {
            ++*vocoderCalls;
            return std::make_unique<Vo::VocoderResult>();
        });
        arch::AudioInput input;
        input.context = m_context;
        input.piece = piece();
        input.piece.notes[0].pronunciation = "AP";
        input.piece.notes[0].phonemes.clear();
        for (const auto id : {"pitch", "energy", "gender", "velocity", "tone_shift"}) input.parameters.emplace(id, arch::AudioParameter{100, std::nullopt});
        const auto output = await<arch::AudioOutput>([&](auto callback) { m_implementation->audio(input, {}, std::move(callback)); });
        ASSERT_FALSE(output);
        EXPECT_NE(output.error().get_if<arch::OperationError>(), nullptr);
        EXPECT_EQ(acousticCalls->load(), 1U);
        EXPECT_EQ(vocoderCalls->load(), 0U);
    }
}
