#include "DemucsSpectralReference.h"
#include "SpectralTransform.h"
#include "TestFiles.h"
#include "TestCoroutine.h"

#include <dr_wav.h>

#include <chrono>
#include <cmath>
#include <future>
#include <numbers>
#include <thread>

#include <gtest/gtest.h>
#include <json/writer.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsspapiv1/ExtractionMapping.h>
#include <dsspapiv1/Controllers.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dsspcodecutil/codecutil.h>
#include <dsspextraction/SeparationTypes.h>
#include <dsspextraction/Service.h>
#include <dsspinference/ResourceIdentity.h>
#include <dsspmedia/Wave.h>

namespace dssp::tests {
    using namespace std::chrono_literals;
    namespace Sep = srtsepinfer::Api::Separation::L1;

    namespace {
        constexpr char Id[] = "example/separation=1.0:inference/demucs";

        drogon::HttpRequestPtr separationRequest(const Json::Value &body) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Post);
            request->addHeader("content-type", "application/json");
            request->setBody(Json::writeString(Json::StreamWriterBuilder{}, body));
            return request;
        }

        std::string wave(int rate = 44100, int channels = 2) {
            std::vector<float> samples(501 * channels);
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = (i % channels == 0) ? 0.25f : -0.125f;

            const drwav_data_format format{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, static_cast<drwav_uint32>(channels), static_cast<drwav_uint32>(rate), 32};
            void *bytes = nullptr;
            std::size_t size = 0;
            drwav writer{};
            EXPECT_TRUE(drwav_init_memory_write(&writer, &bytes, &size, &format, nullptr));
            EXPECT_EQ(drwav_write_pcm_frames(&writer, 501, samples.data()), 501);
            EXPECT_EQ(drwav_uninit(&writer), DRWAV_SUCCESS);

            const codecutil::Base64EncodingView encoded(std::span(static_cast<const std::uint8_t *>(bytes), size));
            auto result = "data:audio/wav;base64," + std::string(encoded.begin(), encoded.end());
            drwav_free(bytes, nullptr);
            return result;
        }

        Json::Value mappedDeclaration(const std::vector<std::string> &modelTracks, const std::vector<std::pair<std::string, std::string>> &tracks) {
            Json::Value declaration;
            declaration["name"] = "Mapped Demucs";
            declaration["interface"] = Sep::API_INTERFACE;
            declaration["variant"] = "demucs";
            declaration["level"] = 1;

            declaration["exports"]["sampleRate"] = 44100;
            declaration["exports"]["channelCount"] = 2;

            auto &configuration = declaration["configuration"];
            configuration["model"] = "missing.onnx";
            configuration["segmentSamples"] = 343980;
            configuration["fftSize"] = 4096;
            configuration["hopLength"] = 1024;
            configuration["overlap"] = .25;

            for (const auto &track : modelTracks)
                configuration["modelTracks"].append(track);
            for (const auto &[id, source] : tracks) {
                Json::Value track;
                track["id"] = id;
                track["description"] = id;
                declaration["exports"]["tracks"].append(track);
                configuration["trackMapping"][id] = source;
            }

            return declaration;
        }

        auto loadMappedDeclaration(const TestFiles &files, const Json::Value &declaration) {
            files.write("package/desc.json", R"({"$version":"1.0","id":"example/mapped-separation","version":"1","runtimeLevel":1,"contributions":{"inference":[{"id":"demucs","path":"demucs.json"}]}})");
            files.write("package/demucs.json", Json::writeString(Json::StreamWriterBuilder{}, declaration));

            inference::EnvironmentOptions options;
            options.packageRoots = {files.path()};
            options.pluginPaths = {{"inference", DSSP_TEST_SEPARATION_PLUGIN_DIR}};
            return inference::Environment::create(options);
        }

        class FakeSeparator final : public Sep::SeparationExecutive {
        public:
            FakeSeparator(srt::InferenceSpec &spec, bool invalid, std::shared_ptr<std::promise<void>> started)
                : SeparationExecutive(spec), m_invalid(invalid), m_started(std::move(started)) {}

            ~FakeSeparator() override {
                (void)stop();
                (void)waitForFinished();
            }

        protected:
            srt::Expected<std::unique_ptr<Sep::SeparationResult>> run(const Sep::SeparationStartInput &input) override {
                if (m_started) {
                    m_started->set_value();
                    const auto deadline = std::chrono::steady_clock::now() + 3s;
                    while (!cancelled() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(1ms);
                    if (cancelled())
                        return srtsepinfer::cancelledError();
                    return srt::Error(srtsepinfer::SeparationError::Internal, "Native cancellation did not arrive");
                }

                auto result = std::make_unique<Sep::SeparationResult>();
                for (const auto &id : {"drums", "bass", "other", "vocals"})
                    result->tracks.push_back({id, {input.audio.sampleRate, input.audio.channelCount, input.audio.samples, input.audio.startTime}});
                if (m_invalid)
                    result->tracks.back().audio.samples.pop_back();

                return result;
            }

        private:
            bool m_invalid;
            std::shared_ptr<std::promise<void>> m_started;
        };
    }

    class SeparationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_files.write("package/desc.json", R"({"$version":"1.0","id":"example/separation","version":"1","runtimeLevel":1,"contributions":{"inference":[{"id":"demucs","path":"demucs.json"}]}})");
            m_files.write("package/demucs.json", R"({"name":"Demucs","interface":"org.openvpi.srtsepinfer.inference.Separation","variant":"demucs","level":1,"exports":{"sampleRate":44100,"channelCount":2,"tracks":[{"id":"drums","description":{"_":"Drums","zh-CN":"鼓"}},{"id":"bass","description":"Bass"},{"id":"other","description":"Other"},{"id":"vocals","description":"Vocals"}]},"configuration":{"model":"missing.onnx","segmentSamples":343980,"fftSize":4096,"hopLength":1024,"overlap":0.25}})");

            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            options.pluginPaths = {{"inference", DSSP_TEST_SEPARATION_PLUGIN_DIR}};
            auto made = inference::Environment::create(options);
            ASSERT_TRUE(made);
            m_environment = *made;
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
        }

        void TearDown() override {
            if (m_service)
                m_service->shutdown();
            if (m_environment)
                m_environment->shutdown();
        }

        void install(bool invalid = false, std::shared_ptr<std::promise<void>> started = {}) {
            auto *category = dynamic_cast<srt::InferenceCategory *>(m_environment->unit().category("inference"));
            ASSERT_NE(category, nullptr);
            auto specs = category->inferences();
            ASSERT_EQ(specs.size(), 1);
            auto *spec = specs.front();
            auto pool = m_environment->inferences("extraction-separation");
            ASSERT_TRUE(pool);

            inference::InferenceDefinition definition;
            definition.key = inference::resourceReference(*spec) + "/default";
            definition.packages = {spec->package()};
            definition.create = [spec, invalid, started]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> { return std::make_unique<FakeSeparator>(*spec, invalid, started); };
            ASSERT_TRUE((*pool)->bind(std::move(definition)));
        }

        void start() {
            auto made = extraction::Service::create(m_environment);
            ASSERT_TRUE(made);
            m_service = *made;
        }

        auto submit(extraction::SeparationInput input, std::stop_token token = {}) {
            auto promise = std::make_shared<std::promise<extraction::ExtractionResult<extraction::SeparationOutput>>>();
            auto future = promise->get_future();
            m_service->separation(std::move(input), token, [promise](auto result) { promise->set_value(std::move(result)); });
            return future;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        std::shared_ptr<extraction::Service> m_service;
    };

    TEST_F(SeparationTest, ColdDiscoveryExportsLocalizedOrderedTracksWithoutLoadingModel) {
        start();

        auto list = m_service->extractors(localization::Context({}, "zh-CN"));
        ASSERT_TRUE(list);
        ASSERT_EQ(list->separation.size(), 1);
        EXPECT_TRUE(list->note.empty());
        EXPECT_TRUE(list->pitch.empty());

        const auto &info = list->separation.front();
        EXPECT_EQ(info.id, Id);
        ASSERT_EQ(info.tracks.size(), 4);
        EXPECT_EQ(info.tracks.front().name, "drums");
        EXPECT_EQ(info.tracks.front().description, "鼓");

        api_v1::ExtractionController controller(std::make_shared<http::RequestLifetimeRegistry>(), m_service);
        auto request = drogon::HttpRequest::newHttpRequest();
        request->addHeader("accept-language", "zh-CN");
        drogon::HttpResponsePtr listResponse, metadataResponse;
        controller.extractors(request, [&](const auto &response) { listResponse = response; });
        controller.separationExtractor(request, [&](const auto &response) { metadataResponse = response; }, Id);

        ASSERT_NE(listResponse, nullptr);
        ASSERT_NE(metadataResponse, nullptr);
        ASSERT_EQ(listResponse->statusCode(), drogon::k200OK);
        ASSERT_EQ(metadataResponse->statusCode(), drogon::k200OK);
        ASSERT_NE(listResponse->getJsonObject(), nullptr);
        ASSERT_NE(metadataResponse->getJsonObject(), nullptr);

        const auto &json = *metadataResponse->getJsonObject();
        EXPECT_EQ((*listResponse->getJsonObject())["separation"][0], json);
        EXPECT_EQ(json.getMemberNames(), (std::vector<std::string>{"acceptableAudioSampleRates", "acceptableFormats", "acceptableSchemes", "id", "name", "preferredAudioSampleRate", "tracks"}));
        EXPECT_EQ(json["id"].asString(), Id);
        EXPECT_EQ(json["preferredAudioSampleRate"].asUInt(), 44100);
        ASSERT_TRUE(json["tracks"].isArray());
        ASSERT_EQ(json["tracks"].size(), info.tracks.size());
        for (Json::ArrayIndex i = 0; i < json["tracks"].size(); ++i) {
            EXPECT_EQ(json["tracks"][i].getMemberNames(), (std::vector<std::string>{"name"}));
            EXPECT_EQ(json["tracks"][i]["name"].asString(), info.tracks[i].name);
        }

        auto pool = m_environment->inferences("extraction-separation");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0);
        EXPECT_FALSE(m_service->extractor(extraction::Kind::Pitch, Id));
    }

    TEST_F(SeparationTest, PreservesStereoAndPartialTailInEveryWaveOutput) {
        install();
        start();

        auto future = submit({{Id, wave()}});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        auto result = future.get();

        ASSERT_TRUE(result) << result.error().asError().formatChain();
        ASSERT_EQ(result->tracks.size(), 4);
        for (const auto &track : result->tracks) {
            auto decoded = media::readWaveDataUrl(track.audioUrl);
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->channelCount, 2);
            EXPECT_EQ(decoded->sampleRate, 44100);
            ASSERT_EQ(decoded->samples.size(), 1002);
            EXPECT_FLOAT_EQ(decoded->samples[1000], 0.25f);
            EXPECT_FLOAT_EQ(decoded->samples[1001], -0.125f);
        }
    }

    TEST_F(SeparationTest, RejectsWrongFormatBeforeOpeningModel) {
        start();

        for (const auto &input : std::vector<extraction::SeparationInput>{{{Id, wave(48000)}}, {{Id, wave(44100, 1)}}, {{Id, "https://example.invalid/audio.wav"}}, {{"missing", wave()}}}) {
            auto future = submit(input);
            ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
            auto result = future.get();
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().get_if<extraction::InvalidInputError>(), nullptr);
        }

        auto pool = m_environment->inferences("extraction-separation");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0);
    }

    TEST_F(SeparationTest, RejectsMalformedNativeTrack) {
        install(true);
        start();

        auto future = submit({{Id, wave()}});
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        auto result = future.get();

        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::OperationError>(), nullptr);
    }

    TEST_F(SeparationTest, CancellationReachesNativeSeparatorAndReturnsNoTracks) {
        auto started = std::make_shared<std::promise<void>>();
        auto entered = started->get_future();
        install(false, started);
        start();

        std::stop_source stop;
        auto future = submit({{Id, wave()}}, stop.get_token());
        ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);

        stop.request_stop();
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);

        auto result = future.get();
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().get_if<extraction::CancelledError>(), nullptr);
    }

    TEST_F(SeparationTest, HttpNegotiationAcceptsDefaultsEmptyListsAndSupportedFallbacks) {
        install();
        start();
        api_v1::ExtractionController controller(std::make_shared<http::RequestLifetimeRegistry>(), m_service);

        for (int variant = 0; variant < 3; ++variant) {
            SCOPED_TRACE(variant);
            Json::Value body;
            body["extractor"] = Id;
            body["input"]["audioUrl"] = wave();
            if (variant != 0) {
                body["acceptableFormats"] = Json::Value(Json::arrayValue);
                body["acceptableSchemes"] = Json::Value(Json::arrayValue);
            }
            if (variant == 2) {
                body["acceptableFormats"].append("audio/flac");
                body["acceptableFormats"].append("Audio/WAV");
                body["acceptableSchemes"].append("https");
                body["acceptableSchemes"].append("DATA");
            }

            auto promise = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
            auto future = promise->get_future();
            startCoroutine(controller.separation(separationRequest(body), [promise](const auto &response) { promise->set_value(response); }));
            ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
            const auto response = future.get();

            ASSERT_EQ(response->statusCode(), drogon::k200OK) << response->body();
            const auto json = response->getJsonObject();
            ASSERT_NE(json, nullptr);
            const auto &tracks = (*json)["output"]["tracks"];
            ASSERT_EQ(tracks.size(), 4);
            for (const auto &track : tracks) {
                const auto url = track["audioUrl"].asString();
                EXPECT_TRUE(url.starts_with("data:audio/wav;base64,"));
                auto decoded = media::readWaveDataUrl(url);
                ASSERT_TRUE(decoded);
                EXPECT_EQ(decoded->sampleRate, 44100);
                EXPECT_EQ(decoded->channelCount, 2);
                EXPECT_EQ(decoded->samples.size(), 1002);
            }
        }
    }

    TEST_F(SeparationTest, HttpNegotiationRejectsUnsupportedOutputBeforeDecodingOrOpeningModel) {
        start();
        api_v1::ExtractionController controller(std::make_shared<http::RequestLifetimeRegistry>(), m_service);

        for (const auto field : {"acceptableFormats", "acceptableSchemes"}) {
            SCOPED_TRACE(field);
            Json::Value body;
            body["extractor"] = Id;
            body["input"]["audioUrl"] = "data:audio/wav;base64,broken";
            body[field] = Json::Value(Json::arrayValue);
            body[field].append(std::string_view(field) == "acceptableFormats" ? "audio/flac" : "https");

            auto promise = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
            auto future = promise->get_future();
            startCoroutine(controller.separation(separationRequest(body), [promise](const auto &response) { promise->set_value(response); }));
            ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
            const auto response = future.get();

            ASSERT_EQ(response->statusCode(), drogon::k422UnprocessableEntity) << response->body();
            const auto json = response->getJsonObject();
            ASSERT_NE(json, nullptr);
            EXPECT_TRUE((*json)["detail"].asString().starts_with(std::string(field) + " does not accept the separated audio"));
        }

        auto pool = m_environment->inferences("extraction-separation");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0);
    }

    TEST(DemucsConfiguration, AcceptsSubsetsRenamingReorderingAndLegacySixSourcesWithoutLoadingModel) {
        const std::vector<std::string> four{"drums", "bass", "other", "vocals"};
        const std::vector<std::string> six{"drums", "bass", "other", "vocals", "guitar", "piano"};

        auto legacy = mappedDeclaration(six, {{"drums", "drums"}, {"bass", "bass"}, {"other", "other"}, {"vocals", "vocals"}, {"guitar", "guitar"}, {"piano", "piano"}});
        legacy["configuration"].removeMember("modelTracks");
        legacy["configuration"].removeMember("trackMapping");

        for (const auto &declaration : {mappedDeclaration(four, {{"vocals", "vocals"}}), mappedDeclaration(four, {{"lead", "vocals"}, {"percussion", "drums"}}), mappedDeclaration(six, {{"keys", "piano"}, {"strings", "guitar"}}), legacy}) {
            SCOPED_TRACE(Json::writeString(Json::StreamWriterBuilder{}, declaration));
            TestFiles files;
            auto host = loadMappedDeclaration(files, declaration);
            ASSERT_TRUE(host);
            ASSERT_TRUE((*host)->problems().empty());
            ASSERT_EQ((*host)->packages().size(), 1);

            auto service = extraction::Service::create(*host);
            ASSERT_TRUE(service);
            auto list = (*service)->extractors();
            ASSERT_TRUE(list);
            ASSERT_EQ(list->separation.size(), 1);

            const auto &tracks = list->separation.front().tracks;
            ASSERT_EQ(tracks.size(), declaration["exports"]["tracks"].size());
            for (Json::ArrayIndex i = 0; i < tracks.size(); ++i)
                EXPECT_EQ(tracks[i].name, declaration["exports"]["tracks"][i]["id"].asString());

            auto pool = (*host)->inferences("extraction-separation");
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0);

            (*service)->shutdown();
            (*host)->shutdown();
        }
    }

    TEST(DemucsConfiguration, RejectsIncompleteOrInvalidMappingsDuringPackageLoad) {
        const auto valid = mappedDeclaration({"drums", "bass", "other", "vocals"}, {{"lead", "vocals"}, {"percussion", "drums"}});
        for (int variant = 0; variant < 10; ++variant) {
            SCOPED_TRACE(variant);
            auto declaration = valid;
            auto &config = declaration["configuration"];
            switch (variant) {
            case 0: config.removeMember("modelTracks"); break;
            case 1: config.removeMember("trackMapping"); break;
            case 2: config["trackMapping"].removeMember("lead"); break;
            case 3: config["trackMapping"]["unknown"] = "bass"; break;
            case 4: config["trackMapping"]["lead"] = "piano"; break;
            case 5: config["trackMapping"]["lead"] = 3; break;
            case 6: config["trackMapping"] = Json::Value(Json::arrayValue); break;
            case 7: config["modelTracks"][0] = "vocals"; break;
            case 8: config["modelTracks"][0] = 0; break;
            case 9:
                config.removeMember("modelTracks");
                config.removeMember("trackMapping");
                break;
            }

            TestFiles files;
            auto host = loadMappedDeclaration(files, declaration);
            ASSERT_TRUE(host);
            EXPECT_TRUE((*host)->packages().empty());
            ASSERT_EQ((*host)->problems().size(), 1);
            EXPECT_FALSE((*host)->problems().front().error.message().empty());

            (*host)->shutdown();
        }
    }

    TEST(DemucsSpectral, MatchesOfficialStftAndIstftIncludingBoundaryPadding) {
        constexpr std::size_t length = 8201, frames = 9, bins = 2048;
        std::vector<float> input(2 * length);
        for (int channel = 0; channel < 2; ++channel)
            for (std::size_t i = 0; i < length; ++i)
                input[channel * length + i] = static_cast<float>(std::sin(i * .013 + channel * .3) * .2 + (static_cast<int>((i * 17 + channel * 11) % 31) - 15) * .001);

        srtsepinfer::demucs::SpectralTransform transform;
        auto spectrum = transform.forward(input, 2);
        ASSERT_TRUE(spectrum);
        for (const auto &point : spectralReference)
            EXPECT_NEAR((*spectrum)[((point.channel * 2 + point.part) * bins + point.bin) * frames + point.frame], point.value, 3e-6);

        auto reconstructed = transform.inverse(*spectrum, 2, length);
        ASSERT_TRUE(reconstructed);
        for (const auto &point : audioReference)
            EXPECT_NEAR((*reconstructed)[point.channel * length + point.sample], point.value, 3e-6);

        EXPECT_FALSE(transform.forward(input, 2, [] { return true; }));
        EXPECT_FALSE(transform.inverse(*spectrum, 2, length, [] { return true; }));
    }

    TEST(DemucsSpectral, RealInverseMatchesAnalyticDcAndComplexBinsForFourAndSixStereoTracks) {
        constexpr std::size_t length = 8201, frames = 9, bins = 2048;
        constexpr int size = 4096, hop = 1024, trim = size / 2 + hop * 3 / 2;
        const auto window = [](int i) { return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / size); };
        srtsepinfer::demucs::SpectralTransform transform;

        for (const auto channels : {8, 12}) {
            std::vector<float> spectrum(channels * 2 * bins * frames);
            for (int channel = 0; channel < channels; ++channel) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    spectrum[(channel * 2 * bins) * frames + frame] = static_cast<float>(channel + 1);
                    // Imaginary DC is ignored by a real inverse, as in torch.istft.
                    spectrum[((channel * 2 + 1) * bins) * frames + frame] = 17;
                    for (const auto bin : {37, 2047}) {
                        spectrum[(channel * 2 * bins + bin) * frames + frame] = (channel + 1) * .25f;
                        spectrum[((channel * 2 + 1) * bins + bin) * frames + frame] = -(channel + 1) * .125f;
                    }
                }
            }

            auto audio = transform.inverse(spectrum, channels, length);
            ASSERT_TRUE(audio);
            ASSERT_EQ(audio->size(), channels * length);

            for (int channel = 0; channel < channels; ++channel) {
                for (const auto sample : {0, 1, 1024, 4096, 8200}) {
                    double numerator = 0, denominator = 0;
                    for (int frame = 0; frame < frames + 4; ++frame) {
                        const auto index = sample + trim - frame * hop;
                        if (index >= 0 && index < size)
                            denominator += window(index) * window(index);
                    }

                    for (int frame = 0; frame < frames; ++frame) {
                        const auto index = sample + trim - (frame + 2) * hop;
                        if (index < 0 || index >= size)
                            continue;
                        double value = channel + 1;
                        for (const auto bin : {37, 2047}) {
                            const auto phase = 2 * std::numbers::pi * bin * index / size;
                            value += (channel + 1) * (.5 * std::cos(phase) + .25 * std::sin(phase));
                        }
                        numerator += value / std::sqrt(size) * window(index);
                    }

                    EXPECT_NEAR((*audio)[channel * length + sample], numerator / denominator, 3e-6);
                }
            }
        }
    }
}
