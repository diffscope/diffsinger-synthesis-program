#include "BeatFeatures.h"
#include "BeatFeatureReference.h"
#include "TestFiles.h"

#include <dr_wav.h>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <thread>

#include <gtest/gtest.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsspapiv1/ExtractionMapping.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dsspcodecutil/codecutil.h>
#include <dsspextraction/Service.h>
#include <dsspextraction/BeatTypes.h>
#include <dsspinference/ResourceIdentity.h>

namespace dssp::tests {
    using namespace std::chrono_literals;
    namespace Beat = srtbeatinfer::Api::Beat::L1;

    namespace {
        constexpr char Id[] = "example/beat=1.0:inference/final0";

        std::string wave(int rate = 22050, int channels = 2) {
            std::vector<float> samples(rate * channels);
            for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = i % channels == 0 ? 0.25f : -0.125f;

            const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, static_cast<drwav_uint32>(channels), static_cast<drwav_uint32>(rate), 32};
            void *bytes = nullptr;
            std::size_t size = 0;
            drwav writer{};
            EXPECT_TRUE(drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr));
            EXPECT_EQ(drwav_write_pcm_frames(&writer, rate, samples.data()), rate);
            EXPECT_EQ(drwav_uninit(&writer), DRWAV_SUCCESS);

            const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
            auto result = "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
            drwav_free(bytes, nullptr);
            return result;
        }

        class FakeTracker final : public Beat::BeatExecutive {
        public:
            FakeTracker(srt::InferenceSpec &spec, int mode, std::shared_ptr<std::promise<void>> started) : BeatExecutive(spec), m_mode(mode), m_started(std::move(started)) {}

            ~FakeTracker() override {
                (void)stop();
                (void)waitForFinished();
            }

        protected:
            srt::Expected<std::unique_ptr<Beat::BeatResult>> run(const Beat::BeatStartInput &input) override {
                EXPECT_EQ(input.audio.channelCount, 1);
                EXPECT_EQ(input.audio.sampleRate, 22050);
                EXPECT_EQ(input.audio.samples.size(), 22050);
                EXPECT_EQ(input.audio.startTime, 0);
                EXPECT_FLOAT_EQ(input.audio.samples[500], 0.0625f);

                if (m_started) {
                    m_started->set_value();
                    const auto deadline = std::chrono::steady_clock::now() + 3s;
                    while (!cancelled() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
                    if (cancelled()) return srtbeatinfer::cancelledError();
                    return srt::Error(srtbeatinfer::BeatError::Internal, "Native cancellation did not arrive");
                }

                auto result = std::make_unique<Beat::BeatResult>();
                result->beats = {{0.25, true}, {0.75, false}};
                if (m_mode == 1) result->beats[1].time = 0.1;
                if (m_mode == 2) result->beats[1].time = std::numeric_limits<double>::quiet_NaN();
                if (m_mode == 3) result->beats[1].time = 1.1;
                if (m_mode == 4) result->beats.clear();

                return result;
            }

        private:
            int m_mode;
            std::shared_ptr<std::promise<void>> m_started;
        };
    }

    class BeatTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_files.write("package/desc.json", R"({"$version":"1.0","id":"example/beat","version":"1","runtimeLevel":1,"contributions":{"inference":[{"id":"final0","path":"final0.json"}]}})");
            m_files.write("package/final0.json", R"({"name":{"_":"Beat This!","zh-CN":"节拍提取"},"interface":"org.openvpi.srtbeatinfer.inference.Beat","variant":"beat-this","level":1,"exports":{"sampleRate":22050,"channelCount":1,"maxSegmentDuration":0,"downbeats":true},"configuration":{"model":"missing.onnx","chunkFrames":1500,"borderFrames":6}})");

            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            options.pluginPaths = {{"inference", DSSP_TEST_BEAT_PLUGIN_DIR}};
            auto made = inference::Environment::create(options);
            ASSERT_TRUE(made);
            m_environment = *made;
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
        }

        void TearDown() override {
            if (m_service) m_service->shutdown();
            if (m_environment) m_environment->shutdown();
        }

        void start(int mode = 0, std::shared_ptr<std::promise<void>> started = {}, bool install = true) {
            if (install) {
                auto *category = dynamic_cast<srt::InferenceCategory *>(m_environment->unit().category("inference"));
                ASSERT_NE(category, nullptr);
                auto specs = category->inferences();
                ASSERT_EQ(specs.size(), 1);
                auto *spec = specs.front();
                auto pool = m_environment->inferences("extraction-tempo");
                ASSERT_TRUE(pool);

                inference::InferenceDefinition definition;
                definition.key = inference::resourceReference(*spec) + "/default";
                definition.packages = {spec->package()};
                definition.create = [spec, mode, started]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> { return std::make_unique<FakeTracker>(*spec, mode, started); };
                ASSERT_TRUE((*pool)->bind(std::move(definition)));
            }

            auto made = extraction::Service::create(m_environment);
            ASSERT_TRUE(made);
            m_service = *made;
        }

        auto submit(extraction::Input input, std::stop_token token = {}) {
            auto promise = std::make_shared<std::promise<extraction::ExtractionResult<extraction::TempoOutput>>>();
            auto future = promise->get_future();
            m_service->tempo(std::move(input), token, [promise](auto result) { promise->set_value(std::move(result)); });
            return future;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        std::shared_ptr<extraction::Service> m_service;
    };

    TEST_F(BeatTest, ColdDiscoveryDoesNotOpenModelAndExportsTempoMetadata) {
        start(0, {}, false);

        auto list = m_service->extractors(localization::Context({}, "zh-CN"));
        ASSERT_TRUE(list);
        ASSERT_EQ(list->tempo.size(), 1);
        EXPECT_EQ(list->tempo[0].id, Id);
        EXPECT_EQ(list->tempo[0].name, "节拍提取");
        EXPECT_EQ(list->tempo[0].sampleRate, 22050);

        const auto json = api_v1::toJson(api_v1::toDto(*list));
        EXPECT_EQ(json["tempo"][0]["preferredAudioSampleRate"].asUInt(), 22050);

        auto pool = m_environment->inferences("extraction-tempo");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0);
        EXPECT_FALSE(m_service->extractor(extraction::Kind::Note, Id));
    }

    TEST_F(BeatTest, DownmixesAndReturnsAbsolutePositionsWithBooleanDownbeats) {
        start();

        auto future = submit({Id, wave()});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        auto result = future.get();
        ASSERT_TRUE(result) << result.error().asError().formatChain();

        auto json = api_v1::toJson(api_v1::dto::TempoExtractionResponse{api_v1::toDto(std::move(*result))});
        EXPECT_EQ(json["output"]["beats"][0]["position"].asDouble(), 0.25);
        EXPECT_EQ(json["output"]["beats"][1]["position"].asDouble(), 0.75);
        EXPECT_TRUE(json["output"]["beats"][0]["downbeat"].asBool());
        EXPECT_FALSE(json["output"]["beats"][1]["downbeat"].asBool());
        EXPECT_FALSE(json["output"].isMember("tempo"));
        EXPECT_FALSE(json["output"]["beats"][0].isMember("gap"));
    }

    TEST_F(BeatTest, RejectsUnknownExtractorWrongRateAndMalformedAudio) {
        start();

        for (auto input : {extraction::Input{"unknown", wave()}, extraction::Input{Id, wave(44100)}, extraction::Input{Id, "data:audio/wav;base64,AAAA"}}) {
            auto future = submit(std::move(input));
            ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
            auto result = future.get();
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().get_if<extraction::InvalidInputError>(), nullptr);
        }
    }

    TEST_F(BeatTest, RejectsUnorderedNativeTimestamps) {
        start(1);

        auto future = submit({Id, wave()});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        auto result = future.get();
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::OperationError>(), nullptr);
    }

    TEST_F(BeatTest, RejectsNonfiniteNativeTimestamps) {
        start(2);

        auto future = submit({Id, wave()});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        EXPECT_FALSE(future.get());
    }

    TEST_F(BeatTest, RejectsNativeTimestampsOutsideInput) {
        start(3);

        auto future = submit({Id, wave()});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        EXPECT_FALSE(future.get());
    }

    TEST_F(BeatTest, EmptyDetectionRemainsAnArray) {
        start(4);

        auto future = submit({Id, wave()});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        auto result = future.get();
        ASSERT_TRUE(result);

        auto json = api_v1::toJson(api_v1::toDto(std::move(*result)));
        EXPECT_TRUE(json["beats"].isArray());
        EXPECT_TRUE(json["beats"].empty());
    }

    TEST_F(BeatTest, CancellationStopsActiveNativeTracker) {
        auto started = std::make_shared<std::promise<void>>();
        auto ready = started->get_future();
        start(0, started);

        std::stop_source stop;
        auto future = submit({Id, wave()}, stop.get_token());
        ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);

        stop.request_stop();
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);

        auto result = future.get();
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::CancelledError>(), nullptr);
    }

    TEST(BeatFeatures, MatchesIndependentTorchLogMelReferenceIncludingReflectedEdges) {
        srtbeatinfer::beatthis::Features features;
        auto result = features.compute(beatReferenceAudio());
        ASSERT_TRUE(result);
        ASSERT_EQ(result->size(), 5 * 128);
        for (const auto &[index, value] : BeatReferenceProbes) EXPECT_NEAR((*result)[index], value, 0.00015) << index;
    }

    TEST(BeatFeatures, HandlesSilenceVeryShortClipsAndCancellation) {
        srtbeatinfer::beatthis::Features features;
        auto result = features.compute(std::vector<float>(1, 0));
        ASSERT_TRUE(result);
        EXPECT_EQ(result->size(), 128);
        for (const auto value : *result) EXPECT_EQ(value, 0);

        EXPECT_FALSE(features.compute(std::vector<float>(1000, 0), [] { return true; }));
    }

    TEST(BeatFeatures, UsesOfficialChunkBordersAndAvoidsShortFinalChunk) {
        using srtbeatinfer::beatthis::chunkStarts;

        EXPECT_EQ(chunkStarts(25), (std::vector<std::ptrdiff_t>{-6}));
        EXPECT_EQ(chunkStarts(1488), (std::vector<std::ptrdiff_t>{-6}));
        EXPECT_EQ(chunkStarts(1489), (std::vector<std::ptrdiff_t>{-6, -5}));
        EXPECT_EQ(chunkStarts(3001), (std::vector<std::ptrdiff_t>{-6, 1482, 1507}));
    }

    TEST(BeatFeatures, DeduplicatesPeaksAndSnapsDownbeatsWithEarlierTieBreaking) {
        std::vector<float> beat(50, -2), downbeat(50, -2);
        beat[5] = beat[6] = 2;
        beat[20] = beat[30] = 3;
        downbeat[25] = 3;

        auto result = srtbeatinfer::beatthis::postprocess(beat, downbeat, 4, 1);

        ASSERT_EQ(result.size(), 3);
        EXPECT_DOUBLE_EQ(result[0].time, 4.11);
        EXPECT_DOUBLE_EQ(result[1].time, 4.4);
        EXPECT_TRUE(result[1].downbeat);
        EXPECT_FALSE(result[2].downbeat);
    }
}
