#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#include <type_traits>
#include <system_error>

#include <dr_wav.h>
#include <gtest/gtest.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsspcodecutil/codecutil.h>
#include <dsspextraction/OtterTypes.h>
#include <dsspextraction/Service.h>
#include <dsspinference/ResourceIdentity.h>
#include <dsspresource/TaskProgress.h>
#include <dssptests/TestAnalysisPackage.h>

namespace dssp::tests {
    using namespace std::chrono_literals;
    namespace Note = otter::Api::Note::L1;
    namespace F0 = otter::Api::F0::L1;

    namespace {
        std::string wave(int rate, int channels, std::size_t frames) {
            std::vector<float> samples(frames * channels, 0.5f);
            if (channels == 2) {
                for (std::size_t frame = 0; frame < frames; ++frame) samples[frame * 2 + 1] = -0.25f;
            }

            const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, static_cast<drwav_uint32>(channels), static_cast<drwav_uint32>(rate), 32};
            void *bytes = nullptr;
            std::size_t size = 0;
            drwav writer{};
            if (!drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr)) return {};
            EXPECT_EQ(drwav_write_pcm_frames(&writer, frames, samples.data()), frames);
            EXPECT_EQ(drwav_uninit(&writer), DRWAV_SUCCESS);

            const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
            const auto url = "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
            drwav_free(bytes, nullptr);
            return url;
        }

        class TestNoteExecutive final : public Note::NoteExecutive {
        public:
            explicit TestNoteExecutive(srt::InferenceSpec &spec) : NoteExecutive(spec) {}

        protected:
            srt::Expected<std::unique_ptr<Note::NoteResult>> run(const Note::NoteStartInput &input) override {
                EXPECT_EQ(input.audio.channelCount, 1);
                EXPECT_FALSE(input.language.has_value());
                EXPECT_TRUE(input.knownNotes.empty());
                for (const auto value : input.audio.samples) EXPECT_FLOAT_EQ(value, 0.125f);
                if (input.progress) {
                    EXPECT_TRUE(input.progress(0));
                    EXPECT_TRUE(input.progress(0.3));
                    EXPECT_TRUE(input.progress(0.8));
                }

                auto result = std::make_unique<Note::NoteResult>();
                result->notes.push_back({60, input.audio.startTime, static_cast<double>(input.audio.samples.size()) / input.audio.sampleRate, 1});
                if (input.progress) EXPECT_TRUE(input.progress(1));
                return result;
            }
        };

        class TestPitchExecutive final : public F0::F0Executive {
        public:
            TestPitchExecutive(srt::InferenceSpec &spec, std::shared_ptr<std::promise<void>> started = {}) : F0Executive(spec), m_started(std::move(started)) {}

        protected:
            srt::Expected<std::unique_ptr<F0::F0Result>> run(const F0::F0StartInput &input) override {
                if (m_started) {
                    m_started->set_value();
                    const auto deadline = std::chrono::steady_clock::now() + 2s;
                    while (!cancelled() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
                    if (cancelled()) return srt::Error(std::make_error_code(std::errc::operation_canceled), "Test analyzer was stopped");
                    ADD_FAILURE() << "Native stop did not reach the analyzer";
                }

                auto result = std::make_unique<F0::F0Result>();
                result->interval = 0.01;
                result->startTime = input.audio.startTime;
                result->f0.resize(static_cast<std::size_t>(std::ceil(input.audio.samples.size() / 160.0)), 440);
                result->voiced.resize(result->f0.size(), 1);
                return result;
            }

        private:
            std::shared_ptr<std::promise<void>> m_started;
        };
    }

    class ExtractionServiceTest : public ::testing::Test {
    protected:
        void SetUp() override {
            writeAnalysisPackage(m_files, "package");

            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            options.pluginPaths = {{"inference", std::filesystem::path(DSSP_TEST_PLUGIN_DIR) / "otter" / "inferenceinterpreters"}};
            options.cacheGroups = {{"extraction-note", {1, 0ms, 60s}}, {"extraction-pitch", {2, 0ms, 60s}}};

            const auto created = inference::Environment::create(options);
            ASSERT_TRUE(created);
            m_environment = *created;
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
        }

        void TearDown() override {
            if (m_service) m_service->shutdown();
            if (m_environment) m_environment->shutdown();
        }

        void startService() {
            const auto created = extraction::Service::create(m_environment, 0);
            ASSERT_TRUE(created);
            m_service = *created;
        }

        template <class Executive>
        void install(std::string_view id, const std::string &group, std::shared_ptr<std::promise<void>> started = {}) {
            auto *category = dynamic_cast<srt::InferenceCategory *>(m_environment->unit().category(srt::InferenceCategory::NAME));
            ASSERT_NE(category, nullptr);
            srt::InferenceSpec *target = nullptr;
            for (auto *spec : category->inferences()) {
                if (spec->locator().contributionId() == id) target = spec;
            }

            ASSERT_NE(target, nullptr);
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);

            inference::InferenceDefinition definition;
            definition.key = inference::resourceReference(*target) + "/default";
            definition.label = definition.key;
            definition.packages = {target->package()};
            definition.create = [target, started, loaded = m_loaded]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                ++*loaded;
                if constexpr (std::is_same_v<Executive, TestPitchExecutive>) return std::make_unique<Executive>(*target, started);
                else return std::make_unique<Executive>(*target);
            };

            ASSERT_TRUE((*pool)->bind(std::move(definition)));
        }

        template <class Output, class Submit>
        extraction::ExtractionResult<Output> await(Submit submit) {
            const auto promise = std::make_shared<std::promise<extraction::ExtractionResult<Output>>>();
            auto future = promise->get_future();
            submit([promise](auto result) { promise->set_value(std::move(result)); });
            if (future.wait_for(3s) != std::future_status::ready) {
                ADD_FAILURE() << "Extraction callback timed out";
                return fail<extraction::ExtractionErrors>(extraction::OperationError("Test callback timed out"));
            }

            return future.get();
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        std::shared_ptr<extraction::Service> m_service;
        std::shared_ptr<std::atomic<unsigned>> m_loaded = std::make_shared<std::atomic<unsigned>>(0);
    };

    TEST_F(ExtractionServiceTest, EnumeratesTypedMetadataWithoutOpeningAbsentModels) {
        startService();
        ASSERT_NE(m_service, nullptr);

        const auto list = m_service->extractors(localization::Context({}, "zh-CN"));
        ASSERT_TRUE(list);
        ASSERT_EQ(list->note.size(), 1U);
        ASSERT_EQ(list->pitch.size(), 1U);
        EXPECT_EQ(list->note[0].id, analysisNoteId);
        EXPECT_EQ(list->pitch[0].id, analysisPitchId);
        EXPECT_EQ(list->note[0].name, "音符");
        EXPECT_EQ(list->note[0].sampleRate, 44100U);
        EXPECT_EQ(list->pitch[0].sampleRate, 16000U);

        for (const auto group : {"extraction-note", "extraction-pitch"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }

        EXPECT_FALSE(m_service->extractor(extraction::Kind::Pitch, analysisNoteId));
        EXPECT_FALSE(m_service->extractor(extraction::Kind::Tempo, analysisNoteId));
    }

    TEST_F(ExtractionServiceTest, InvalidAudioAndWrongExtractorKindsDoNotLoadModels) {
        startService();
        ASSERT_NE(m_service, nullptr);
        ASSERT_TRUE(m_service->extractor(extraction::Kind::Note, analysisNoteId));
        ASSERT_TRUE(m_service->extractor(extraction::Kind::Pitch, analysisPitchId));

        for (const auto &input : std::vector<extraction::Input>{{analysisNoteId, "https://example.invalid/test.wav"}, {analysisNoteId, wave(16000, 1, 16)}, {analysisNoteId, wave(44100, 1, 0)}, {analysisPitchId, wave(44100, 1, 16)}}) {
            const auto result = await<extraction::NoteOutput>([&](auto callback) { m_service->note(input, {}, std::move(callback)); });
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().get_if<extraction::InvalidInputError>(), nullptr);
            EXPECT_NE(result.error().asError().message().find(input.extractor), std::string::npos);
        }

        const auto pool = m_environment->inferences("extraction-note");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0U);
    }

    TEST_F(ExtractionServiceTest, NoteDownmixAndPitchSpansUseIndependentResidentResources) {
        install<TestNoteExecutive>("note", "extraction-note");
        install<TestPitchExecutive>("pitch", "extraction-pitch");
        startService();
        ASSERT_NE(m_service, nullptr);

        const auto notes = await<extraction::NoteOutput>([&](auto callback) { m_service->note({analysisNoteId, wave(44100, 2, 5292)}, {}, std::move(callback)); });
        ASSERT_TRUE(notes) << (notes ? "" : notes.error().asError().formatChain());
        ASSERT_EQ(notes->notes.size(), 3U);

        double duration = 0;
        for (const auto &note : notes->notes) {
            EXPECT_NEAR(note.gap, 0, 1e-12);
            EXPECT_EQ(note.cent, 6000U);
            duration += note.duration;
        }

        EXPECT_NEAR(duration, 0.12, 1e-12);

        const auto pitch = await<extraction::PitchOutput>([&](auto callback) { m_service->pitch({analysisPitchId, wave(16000, 1, 1921)}, {}, std::move(callback)); });
        ASSERT_TRUE(pitch) << (pitch ? "" : pitch.error().asError().formatChain());
        ASSERT_EQ(pitch->segments.size(), 1U);
        EXPECT_EQ(pitch->segments[0].gap, 0U);
        EXPECT_EQ(pitch->segments[0].pitch.size(), 13U);
        EXPECT_DOUBLE_EQ(pitch->sampleRate, 100);
        EXPECT_EQ(*m_loaded, 2U);

        for (const auto group : {"extraction-note", "extraction-pitch"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 1U);
        }
    }

    TEST_F(ExtractionServiceTest, NoteProgressUsesSpanLengthsAndNativeModelStages) {
        install<TestNoteExecutive>("note", "extraction-note");
        startService();
        ASSERT_NE(m_service, nullptr);
        std::vector<double> values;
        extraction::Input input{analysisNoteId, wave(44100, 2, 5292)};
        input.progress = resource::TaskProgress([&](resource::TaskUpdate update) {
            if (update.state == resource::TaskUpdate::State::Progress) values.push_back(update.value);
        });
        const auto result = await<extraction::NoteOutput>([&](auto callback) { m_service->note(input, {}, std::move(callback)); });
        ASSERT_TRUE(result) << result.error().asError().formatChain();
        ASSERT_EQ(result->notes.size(), 3U);
        std::vector<double> expected{0};
        double completed = 0;
        for (const auto &note : result->notes) {
            expected.push_back(completed / 0.12);
            expected.push_back((completed + 0.3 * note.duration) / 0.12);
            expected.push_back((completed + 0.8 * note.duration) / 0.12);
            completed += note.duration;
            expected.push_back(completed / 0.12);
        }
        ASSERT_EQ(values.size(), expected.size());
        for (std::size_t i = 0; i < values.size(); ++i) EXPECT_NEAR(values[i], expected[i], 1e-12);
    }

    TEST_F(ExtractionServiceTest, NativeStopReachesAnExecutingOtterAnalyzer) {
        const auto started = std::make_shared<std::promise<void>>();
        auto startedFuture = started->get_future();
        install<TestPitchExecutive>("pitch", "extraction-pitch", started);
        startService();
        ASSERT_NE(m_service, nullptr);

        std::stop_source stop;
        const auto promise = std::make_shared<std::promise<extraction::ExtractionResult<extraction::PitchOutput>>>();
        auto future = promise->get_future();
        m_service->pitch({analysisPitchId, wave(16000, 1, 160)}, stop.get_token(), [promise](auto result) { promise->set_value(std::move(result)); });

        ASSERT_EQ(startedFuture.wait_for(1s), std::future_status::ready);

        stop.request_stop();
        ASSERT_EQ(future.wait_for(1s), std::future_status::ready);

        const auto result = future.get();
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::CancelledError>(), nullptr);
    }

    TEST_F(ExtractionServiceTest, NoteEvictionCannotEvictThePitchCache) {
        install<TestNoteExecutive>("note", "extraction-note");
        install<TestPitchExecutive>("pitch", "extraction-pitch");
        startService();
        ASSERT_NE(m_service, nullptr);

        ASSERT_TRUE(await<extraction::NoteOutput>([&](auto callback) { m_service->note({analysisNoteId, wave(44100, 2, 441)}, {}, std::move(callback)); }));
        ASSERT_TRUE(await<extraction::PitchOutput>([&](auto callback) { m_service->pitch({analysisPitchId, wave(16000, 1, 160)}, {}, std::move(callback)); }));

        const auto notePool = m_environment->inferences("extraction-note");
        const auto pitchPool = m_environment->inferences("extraction-pitch");
        ASSERT_TRUE(notePool);
        ASSERT_TRUE(pitchPool);

        auto *category = dynamic_cast<srt::InferenceCategory *>(m_environment->unit().category(srt::InferenceCategory::NAME));
        ASSERT_NE(category, nullptr);
        srt::InferenceSpec *noteSpec = nullptr;
        for (auto *spec : category->inferences()) {
            if (spec->locator().contributionId() == "note") noteSpec = spec;
        }

        ASSERT_NE(noteSpec, nullptr);

        inference::InferenceDefinition another;
        another.key = "another-note-resource";
        another.label = another.key;
        another.packages = {noteSpec->package()};
        another.create = [noteSpec, loaded = m_loaded]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
            ++*loaded;
            return std::make_unique<TestNoteExecutive>(*noteSpec);
        };

        const auto handle = (*notePool)->bind(std::move(another));
        ASSERT_TRUE(handle);

        const auto input = std::make_shared<Note::NoteStartInput>();
        input->audio.sampleRate = 44100;
        input->audio.channelCount = 1;
        input->audio.samples = {0.125f};

        const auto promise = std::make_shared<std::promise<inference::ExecutionResult<std::unique_ptr<Note::NoteResult>>>>();
        auto future = promise->get_future();
        (*notePool)->run<Note::NoteExecutive>(*handle, {}, [input](auto &executive) { return executive.start(*input); }, [promise](auto result) { promise->set_value(std::move(result)); });

        ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
        ASSERT_TRUE(future.get());
        EXPECT_EQ((*notePool)->residentCount(), 1U);
        EXPECT_EQ((*pitchPool)->residentCount(), 1U);
        EXPECT_EQ(*m_loaded, 3U);

        ASSERT_TRUE(await<extraction::PitchOutput>([&](auto callback) { m_service->pitch({analysisPitchId, wave(16000, 1, 160)}, {}, std::move(callback)); }));
        EXPECT_EQ(*m_loaded, 3U);

        ASSERT_TRUE(await<extraction::NoteOutput>([&](auto callback) { m_service->note({analysisNoteId, wave(44100, 2, 441)}, {}, std::move(callback)); }));
        EXPECT_EQ(*m_loaded, 4U);
    }

    TEST_F(ExtractionServiceTest, NativeInitializationFailureIsAnUnexpectedOperationError) {
        startService();
        ASSERT_NE(m_service, nullptr);

        const auto result = await<extraction::PitchOutput>([&](auto callback) { m_service->pitch({analysisPitchId, wave(16000, 1, 160)}, {}, std::move(callback)); });

        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::OperationError>(), nullptr);
        EXPECT_NE(result.error().asError().message().find(analysisPitchId), std::string::npos);
        EXPECT_NE(result.error().asError().cause(), nullptr);
    }
}
