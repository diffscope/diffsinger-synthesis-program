#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <gtest/gtest.h>
#include <json/json.h>
#include <stdcorelib/path.h>
#include <stdcorelib/scope_guard.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/TcpClient.h>

#include <dsspapiv1/Controllers.h>
#include <dssphttp/AccessLog.h>
#include <dssptests/LogCapture.h>
#include <dssptests/TestArchitecture.h>
#include <dssptests/TestFiles.h>
#include <dssptests/TestAnalysisPackage.h>

namespace dssp::tests {
    namespace {
        class RunningServer {
        public:
            RunningServer(drogon::HttpAppFramework &app, std::shared_ptr<http::AccessLog> access, std::shared_ptr<http::RequestLifetimeRegistry> lifetime) : m_app(app), m_access(std::move(access)), m_lifetime(std::move(lifetime)), m_thread([&app] { app.run(); }) {}

            ~RunningServer() {
                m_access->shutdown();
                m_lifetime->cancelAll();
                m_app.getLoop()->queueInLoop([this] { m_app.quit(); });
                m_thread.join();
            }

        private:
            drogon::HttpAppFramework &m_app;
            const std::shared_ptr<http::AccessLog> m_access;
            const std::shared_ptr<http::RequestLifetimeRegistry> m_lifetime;
            std::thread m_thread;
        };
    }

    // This executable has its own Drogon singleton. It sends real loopback HTTP
    // requests through the production controllers, with a model-free test backend.
    TEST(HttpAccessLog, CoversControllersRoutingErrorsPreflightAndAsyncFailure) {
        using namespace std::chrono_literals;
        LogCapture capture;
        TestFiles files;
        trantor::EventLoopThread backendLoop("test backend"), clientLoop("test HTTP client");
        backendLoop.run();
        clientLoop.run();
        auto implementation = std::make_shared<TestArchitecture>();
        const std::set<std::string> singerIds{
            "Vendor/voices=1.0:singer/main", "Vendor/voices=2.0:singer/main",
            "Vendor/voices=1.0:singer/avatar", "Vendor/voices=1.0:singer/background", "Vendor/voices=1.0:singer/demo-audio",
            "Vendor/voice/packages=1.2.3.4:singer/Lead_Voice-1"
        };

        implementation->onSinger = [&](const arch::ResourceQuery &query) -> arch::CatalogResult<std::optional<arch::SingerInfo>> {
            if (!singerIds.contains(query.id)) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError("Unknown singer: " + query.id));
            }
            arch::SingerInfo singer;
            singer.id = query.id;
            singer.arch = "diffsinger";
            singer.group = "group";
            singer.defaultLanguage = "eng";
            return std::optional{std::move(singer)};
        };

        const auto groupMetadata = [](const arch::CatalogOptions &options) {
            arch::GroupMetadata group;
            group.id = "group";
            group.arch = "diffsinger";
            group.languages.emplace("eng", arch::GroupLanguageInfo{options.acceptLanguage == "ja-JP" ? "英語" : "English", {}, arch::PronunciationMode::Full, arch::PhonemeMode::Full});
            group.languages.emplace("cmn", arch::GroupLanguageInfo{"Mandarin", {}, arch::PronunciationMode::Skip, arch::PhonemeMode::TokenOnly});
            group.durationMode = arch::DurationMode::Full;
            group.parameterPipeline.emplace("pitch", arch::IndirectParameter{{}, arch::ParameterRetakeMode::Range});
            group.audioDependencies = {"pitch"};
            group.archSpecificInfo = Json::Value(Json::arrayValue);
            group.archSpecificInfo.append("shared pipeline");
            group.defaultArchExtra = 7;
            return group;
        };

        implementation->onGroups = [&](const arch::CatalogOptions &options) -> arch::CatalogResult<std::vector<arch::GroupMetadata>> { return std::vector<arch::GroupMetadata>{groupMetadata(options)}; };
        implementation->onGroup = [&](const arch::ResourceQuery &query) -> arch::CatalogResult<std::optional<arch::GroupMetadata>> {
            if (query.id != "group") {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError("Unknown group: " + query.id));
            }
            return std::optional{groupMetadata(query.display)};
        };

        implementation->onAvatar = [&](const arch::SingerMediaQuery &query) -> arch::CatalogResult<std::string> {
            if (!singerIds.contains(query.singer.id)) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError("Unknown singer: " + query.singer.id));
            }
            if (!query.media.acceptableFormats.empty()) {
                EXPECT_EQ(query.media.acceptableFormats, (std::vector<std::string>{"image/png", "application/vnd.example+json"}));
                EXPECT_EQ(query.media.acceptableSchemes, (std::vector<std::string>{"data", "https"}));
            }
            return "data:application/octet-stream;base64," + drogon::utils::base64Encode(query.singer.id);
        };

        implementation->onBackground = [&](const arch::SingerMediaQuery &query) -> arch::CatalogResult<std::string> {
            if (!singerIds.contains(query.singer.id)) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError("Unknown singer: " + query.singer.id));
            }
            return "data:application/octet-stream;base64," + drogon::utils::base64Encode(query.singer.id);
        };

        implementation->onDemoAudio = [&](const arch::SingerMediaQuery &query) -> arch::CatalogResult<std::vector<arch::SingerDemoAudio>> {
            if (!singerIds.contains(query.singer.id)) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError("Unknown singer: " + query.singer.id));
            }
            return std::vector<arch::SingerDemoAudio>{{query.singer.id, "data:audio/wav;base64,AA=="}};
        };

        implementation->onPronunciation = [&](arch::PronunciationInput input, std::stop_token, arch::Completion<arch::PronunciationOutput> complete) {
            const auto fail = input.context.singer.id == "failure";
            backendLoop.getLoop()->queueInLoop([complete = std::move(complete), fail] {
                if (fail) {
                    complete(dssp::fail<arch::SynthesisErrors>(arch::OperationError("test backend failure")));
                } else {
                    complete(arch::PronunciationOutput{});
                }
            });
        };

        auto access = std::make_shared<http::AccessLog>();
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        auto &app = drogon::app();
        app.setThreadNum(1);
        app.setUploadPath(stdc::path::to_utf8(files.path() / "http-cache"));
        app.setDocumentRoot(stdc::path::to_utf8(files.path()));
        access->install(app);
        app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr &request, drogon::AdviceCallback &&respond, drogon::AdviceChainCallback &&next) {
            if (request->method() == drogon::Options) {
                request->attributes()->insert("drogon.corsMethods", std::string("POST"));
                respond(drogon::HttpResponse::newOptionsResponse(request, nullptr, true, true));
            } else {
                next();
            }
        });
        writeAnalysisPackage(files, "analysis-package");
        inference::EnvironmentOptions extractionOptions;
        extractionOptions.packageRoots = {files.path()};
        extractionOptions.pluginPaths = {{"inference", std::filesystem::path(DSSP_TEST_PLUGIN_DIR) / "otter" / "inferenceinterpreters"}};
        const auto extractionEnvironment = inference::Environment::create(extractionOptions);
        ASSERT_TRUE(extractionEnvironment);
        const auto extractionService = extraction::Service::create(*extractionEnvironment);
        ASSERT_TRUE(extractionService);
        const auto releaseExtraction = stdc::make_scope_guard([&] {
            (*extractionService)->shutdown();
            (*extractionEnvironment)->shutdown();
        });
        api_v1::registerControllers(app, lifetime, testCatalog({implementation}), *extractionService);
        app.setConnectionCallback([access, lifetime](const trantor::TcpConnectionPtr &connection) {
            access->connectionChanged(connection);
            lifetime->connectionChanged(connection);
        });
        std::promise<std::uint16_t> portPromise;
        auto portFuture = portPromise.get_future();
        app.addListener("127.0.0.1", 0);
        app.registerBeginningAdvice([&] { portPromise.set_value(app.getListeners().front().toPort()); });
        RunningServer server(app, access, lifetime);
        ASSERT_EQ(portFuture.wait_for(10s), std::future_status::ready);
        const auto port = portFuture.get();
        // Beginning advice exposes the bound port before listen() runs. Await a
        // real connection so the first HTTP request cannot race listener startup.
        const auto readyPromise = std::make_shared<std::promise<void>>();
        auto readyFuture = readyPromise->get_future();
        // TcpClient::connect() uses shared_from_this() for asynchronous callbacks.
        const auto readinessClient = std::make_shared<trantor::TcpClient>(clientLoop.getLoop(), trantor::InetAddress("127.0.0.1", port), "listener readiness");
        readinessClient->setConnectionCallback([readyPromise](const trantor::TcpConnectionPtr &connection) {
            if (connection->connected()) {
                readyPromise->set_value();
            }
        });
        readinessClient->enableRetry();
        readinessClient->connect();
        const auto ready = readyFuture.wait_for(5s);
        readinessClient->stop();
        readinessClient->disconnect();
        ASSERT_EQ(ready, std::future_status::ready);
        auto client = drogon::HttpClient::newHttpClient("127.0.0.1", port, false, clientLoop.getLoop());
        const std::string successBody = R"({"context":{"arch":"diffsinger","archExtra":null,"singer":{"id":"ok","extra":null}},"input":{"notes":[{"lyric":"private-lyric","language":"eng"}]}})";
        const std::string failureBody = R"({"context":{"arch":"diffsinger","archExtra":null,"singer":{"id":"failure","extra":null}},"input":{"notes":[{"lyric":"private-lyric","language":"eng"}]}})";
        const auto noteExtractorPath = "/v1/extractor/note/" + drogon::utils::urlEncodeComponent(analysisNoteId);
        const auto pitchExtractorPath = "/v1/extractor/pitch/" + drogon::utils::urlEncodeComponent(analysisPitchId);
        Json::Value unsupportedAudio;
        unsupportedAudio["extractor"] = analysisNoteId;
        unsupportedAudio["input"]["audioUrl"] = "https://example.invalid/a.wav";
        const auto unsupportedAudioBody = Json::writeString(Json::StreamWriterBuilder{}, unsupportedAudio);
        struct Case {
            drogon::HttpMethod method;
            std::string path;
            std::string body;
            int status;
            std::string_view contentType = "application/json";
            std::string jsonField;
            std::string jsonValue;
            bool jsonArray = false;
            std::string query;
            bool groupMetadata = false;
            std::string acceptLanguage;
        };

        std::vector<Case> cases{
            {drogon::Get, "/.well-known/api-catalog", {}, 200},
            {drogon::Head, "/.well-known/api-catalog", {}, 200},
            {drogon::Post, "/.well-known/api-catalog", {}, 405},
            {drogon::Get, "/v1/openapi.json", {}, 200, "application/json", "openapi", "3.2.0"},
            {drogon::Head, "/v1/openapi.json", {}, 200},
            {drogon::Post, "/v1/openapi.json", {}, 405},
            {drogon::Get, "/v1/info", {}, 200},
            {drogon::Head, "/v1/info", {}, 200},
            {drogon::Get, "/v1/singer", {}, 200},
            {drogon::Get, "/v1/arch/diffsinger", {}, 200},
            {drogon::Get, "/v1/arch/diffsinger/group", {}, 200, "application/json", "id", "group", true, {}, true, "ja-JP"},
            {drogon::Get, "/v1/arch/diffsinger/group/group", {}, 200, "application/json", "id", "group", false, {}, true, "ja-JP"},
            {drogon::Get, "/v1/arch/diffsinger/group/missing", {}, 404},
            {drogon::Get, "/v1/extractor", {}, 200},
            {drogon::Get, noteExtractorPath, {}, 200, "application/json", "id", analysisNoteId},
            {drogon::Get, pitchExtractorPath, {}, 200, "application/json", "id", analysisPitchId},
            {drogon::Get, "/v1/extractor/pitch/" + drogon::utils::urlEncodeComponent(analysisNoteId), {}, 404},
            {drogon::Get, "/v1/extractor/note/" + drogon::utils::urlEncodeComponent(drogon::utils::urlEncodeComponent(analysisNoteId)), {}, 404},
            {drogon::Post, "/v1/extract/note", "{", 400},
            {drogon::Post, "/v1/extract/note", unsupportedAudioBody, 422},
            {drogon::Post, "/v1/extract/tempo", "{}", 422},
            {drogon::Post, "/v1/extract/separation", "{}", 422},
            {drogon::Get, "/v1/arch/unsupported", {}, 404},
            {drogon::Get, "/v1/missing", {}, 404},
            {drogon::Get, "/v1/synth/pronunciation", {}, 405},
            {drogon::Post, "/v1/synth/pronunciation", "{", 400},
            {drogon::Post, "/v1/synth/pronunciation", "text", 415, "text/plain"},
            {drogon::Post, "/v1/synth/pronunciation", "{}", 422},
            {drogon::Post, "/v1/synth/pronunciation", successBody, 200},
            {drogon::Post, "/v1/synth/pronunciation", successBody, 200, "application/json; charset=utf-8"},
            {drogon::Post, "/v1/synth/pronunciation", failureBody, 500},
            {drogon::Options, "/v1/unrouted-preflight", {}, 204},
        };

        for (const auto &id : singerIds) {
            // Encode only the ID component, then send the resulting path verbatim.
            // Encoding the entire path again would turn %2F into %252F.
            const auto path = "/v1/arch/diffsinger/singer/" + drogon::utils::urlEncodeComponent(id);
            const auto mediaUrl = "data:application/octet-stream;base64," + drogon::utils::base64Encode(id);
            cases.push_back({drogon::Get, path, {}, 200, "application/json", "id", id});
            cases.push_back({drogon::Get, path + "/avatar", {}, 200, "application/json", "avatarUrl", mediaUrl});
            cases.push_back({drogon::Get, path + "/background", {}, 200, "application/json", "backgroundUrl", mediaUrl});
            cases.push_back({drogon::Get, path + "/demo-audio", {}, 200, "application/json", "name", id, true});
        }

        cases.push_back({drogon::Get, "/v1/arch/diffsinger/singer/Vendor%2fvoices%3d1.0%3asinger%2fmain", {}, 200, "application/json", "id", "Vendor/voices=1.0:singer/main"});
        cases.push_back({drogon::Get, "/v1/arch/diffsinger/singer/" + drogon::utils::urlEncodeComponent(drogon::utils::urlEncodeComponent("Vendor/voices=1.0:singer/main")), {}, 404});
        cases.push_back({drogon::Get, "/v1/arch/diffsinger/singer/Vendor%252Fvoices%3D1.0%3Asinger%2Fmain", {}, 404});
        const auto queryPath = "/v1/arch/diffsinger/singer/" + drogon::utils::urlEncodeComponent("Vendor/voices=1.0:singer/main") + "/avatar";
        const auto queryMedia = "data:application/octet-stream;base64," + drogon::utils::base64Encode("Vendor/voices=1.0:singer/main");
        const auto query = "acceptableFormats=" + drogon::utils::urlEncodeComponent("image/png") + ',' + drogon::utils::urlEncodeComponent("application/vnd.example+json") + "&acceptableSchemes=data,https";
        cases.push_back({drogon::Get, queryPath, {}, 200, "application/json", "avatarUrl", queryMedia, false, query});
        cases.push_back({drogon::Get, queryPath, {}, 422, "application/json", {}, {}, false, "acceptableFormats=audio%2Fwav%2Caudio%2Fflac"});
        cases.push_back({drogon::Get, queryPath, {}, 422, "application/json", {}, {}, false, "acceptableFormats=application%2Fvnd.example+json"});
        cases.push_back({drogon::Get, queryPath, {}, 422, "application/json", {}, {}, false, "acceptableFormats=application%2Fvnd.example%252Bjson"});
        std::set<std::string> ids;
        std::string catalogLength, specificationLength;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto &item = cases[i];
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(item.method);
            SCOPED_TRACE(std::format("{} {}?{} expected_status={}", request->methodString(), item.path, item.query, item.status));
            const auto requestQuery = item.query.empty() ? "token=private-query" : item.query + "&token=private-query";
            request->setPath(item.path + '?' + requestQuery);
            request->setPathEncode(false);
            if (!item.acceptLanguage.empty()) {
                request->addHeader("accept-language", item.acceptLanguage);
            }
            if (item.method == drogon::Post) {
                // Use the serializer's content-type API, avoiding a second header
                // and form parameters competing with the explicit JSON body.
                request->setContentTypeString(std::string(item.contentType));
                request->setBody(item.body);
            } else if (item.method == drogon::Options) {
                request->addHeader("origin", "https://example.invalid");
                request->addHeader("access-control-request-method", "POST");
            }
            const auto [result, response] = client->sendRequest(request, 5.0);
            ASSERT_EQ(result, drogon::ReqResult::Ok);
            ASSERT_NE(response, nullptr);
            EXPECT_EQ(static_cast<int>(response->statusCode()), item.status);
            if (item.status == 200 && item.path == "/.well-known/api-catalog") {
                EXPECT_EQ(response->contentTypeString(), "application/linkset+json; profile=\"https://www.rfc-editor.org/info/rfc9727\"");
                EXPECT_EQ(response->getHeader("link"), "</.well-known/api-catalog>; rel=\"api-catalog\"; type=\"application/linkset+json\"");
                if (item.method == drogon::Get) {
                    catalogLength = std::to_string(response->body().size());
                    EXPECT_FALSE(response->body().empty());
                } else {
                    EXPECT_TRUE(response->body().empty());
                }
                EXPECT_EQ(response->getHeader("content-length"), catalogLength);
            }
            if (item.status == 200 && item.path == "/v1/openapi.json") {
                EXPECT_EQ(response->contentTypeString(), "application/json");
                if (item.method == drogon::Get) {
                    specificationLength = std::to_string(response->body().size());
                    EXPECT_FALSE(response->body().empty());
                } else {
                    EXPECT_TRUE(response->body().empty());
                }
                EXPECT_EQ(response->getHeader("content-length"), specificationLength);
            }
            if (!item.jsonField.empty()) {
                const auto json = response->getJsonObject();
                ASSERT_NE(json, nullptr);
                if (item.jsonArray) {
                    ASSERT_TRUE(json->isArray());
                    ASSERT_EQ(json->size(), 1U);
                }
                const auto &object = item.jsonArray ? (*json)[0] : *json;
                ASSERT_TRUE(object.isObject());
                ASSERT_TRUE(object[item.jsonField].isString());
                EXPECT_EQ(object[item.jsonField].asString(), item.jsonValue);
                if (item.groupMetadata) {
                    ASSERT_TRUE(object["languages"].isObject());
                    EXPECT_EQ(object["languages"].size(), 2U);
                    EXPECT_EQ(object["languages"]["eng"]["name"].asString(), "英語");
                    EXPECT_EQ(object["languages"]["eng"]["phonemeMode"].asString(), "full");
                    EXPECT_EQ(object["languages"]["cmn"]["pronunciationMode"].asString(), "skip");
                    EXPECT_EQ(object["languages"]["cmn"]["phonemeMode"].asString(), "token_only");
                    EXPECT_EQ(object["durationMode"].asString(), "full");
                    EXPECT_EQ(object["parameterPipeline"]["pitch"]["retakeMode"].asString(), "range");
                    EXPECT_EQ(object["archSpecificInfo"][0].asString(), "shared pipeline");
                    EXPECT_EQ(object["defaultArchExtra"].asInt(), 7);
                    EXPECT_FALSE(object.isMember("pronunciationMode"));
                    EXPECT_FALSE(object.isMember("phonemeMode"));
                } else if (item.jsonField == "id" && item.path.find("/singer/") != std::string::npos) {
                    EXPECT_FALSE(object.isMember("languages"));
                    EXPECT_EQ(object["defaultLanguage"].asString(), "eng");
                }
            }
            if (item.status == 200 && item.path.starts_with("/v1/extractor")) {
                const auto json = response->getJsonObject();
                ASSERT_NE(json, nullptr);
                if (item.path == "/v1/extractor") {
                    EXPECT_TRUE((*json)["tempo"].empty());
                    EXPECT_TRUE((*json)["separation"].empty());
                    EXPECT_EQ((*json)["note"].size(), 1U);
                    EXPECT_EQ((*json)["pitch"].size(), 1U);
                } else {
                    EXPECT_EQ((*json)["acceptableFormats"][0].asString(), "audio/wav");
                    EXPECT_EQ((*json)["acceptableSchemes"][0].asString(), "data");
                    EXPECT_EQ((*json)["acceptableAudioSampleRates"][0], (*json)["preferredAudioSampleRate"]);
                }
            }
            const auto records = capture.accessRecords();
            ASSERT_EQ(records.size(), i + 1);
            const auto &message = records.back().message;
            EXPECT_NE(message.find("method=" + std::string(request->methodString())), std::string::npos);
            EXPECT_NE(message.find("path=" + http::quoteLogValue(drogon::utils::urlDecode(item.path))), std::string::npos);
            EXPECT_NE(message.find("status=" + std::to_string(item.status)), std::string::npos);
            EXPECT_EQ(message.find("private-lyric"), std::string::npos);
            EXPECT_EQ(message.find("private-query"), std::string::npos);
            EXPECT_TRUE(ids.insert(message.substr(0, message.find(" method="))).second);
        }

        const auto failures = capture.records("dssp.api_v1.ArchitectureErrors", stdc::Logger::Critical);
        ASSERT_EQ(failures.size(), 1U);
        const auto failureId = failures.front().message.substr(0, failures.front().message.find(" method="));
        EXPECT_TRUE(std::ranges::any_of(capture.accessRecords(), [&](const auto &record) { return record.message.starts_with(failureId + " ") && record.message.find("status=500") != std::string::npos; }));
    }
}
