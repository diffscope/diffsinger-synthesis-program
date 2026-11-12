#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/drogon.h>
#include <gtest/gtest.h>
#include <json/json.h>
#include <stdcorelib/path.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/TcpClient.h>

#include <dsspapiv1/Controllers.h>
#include <dssphttp/AccessLog.h>
#include <dssplocalization/Catalog.h>
#include <dsspresource/TaskProgress.h>
#include <dssptests/LogCapture.h>
#include <dssptests/TestArchitecture.h>
#include <dssptests/TestFiles.h>

namespace dssp::tests {
    namespace {
        using namespace std::chrono_literals;

        class StreamingServer final {
        public:
            StreamingServer(drogon::HttpAppFramework &app, std::shared_ptr<http::AccessLog> access, std::shared_ptr<http::RequestLifetimeRegistry> lifetime)
                : m_app(app), m_access(std::move(access)), m_lifetime(std::move(lifetime)), m_thread([&app] { app.run(); }) {}

            ~StreamingServer() {
                m_access->shutdown();
                m_lifetime->cancelAll();
                m_app.getLoop()->queueInLoop([this] { m_app.quit(); });
                m_thread.join();
            }

        private:
            drogon::HttpAppFramework &m_app;
            std::shared_ptr<http::AccessLog> m_access;
            std::shared_ptr<http::RequestLifetimeRegistry> m_lifetime;
            std::thread m_thread;
        };

        struct DisconnectProbe {
            std::promise<void> cancelled;
            std::promise<void> completed;
            std::shared_ptr<std::stop_callback<std::function<void()>>> cancellation;
        };

        std::string body(std::string_view singer) {
            Json::Value request;
            request["context"]["arch"] = "diffsinger";
            request["context"]["archExtra"] = Json::nullValue;
            request["context"]["singer"]["id"] = std::string(singer);
            request["context"]["singer"]["extra"] = Json::nullValue;
            Json::Value note;
            note["lyric"] = "la";
            note["language"] = "eng";
            request["input"]["notes"].append(std::move(note));
            return Json::writeString(Json::StreamWriterBuilder{}, request);
        }

        std::vector<Json::Value> events(const drogon::HttpResponse &response) {
            EXPECT_EQ(response.statusCode(), drogon::k200OK);
            EXPECT_TRUE(response.getHeader("content-type").starts_with("application/x-ndjson"));
            EXPECT_EQ(response.getHeader("vary"), "Accept, Accept-Language");
            const auto bytes = response.body();
            EXPECT_FALSE(bytes.empty());
            EXPECT_TRUE(bytes.ends_with('\n'));
            std::vector<Json::Value> result;
            std::size_t begin = 0;
            while (begin < bytes.size()) {
                const auto end = bytes.find('\n', begin);
                if (end == bytes.npos) break;
                EXPECT_GT(end, begin);
                auto reader = std::unique_ptr<Json::CharReader>(Json::CharReaderBuilder{}.newCharReader());
                Json::Value event;
                std::string error;
                EXPECT_TRUE(reader->parse(bytes.data() + begin, bytes.data() + end, &event, &error)) << error;
                result.push_back(std::move(event));
                begin = end + 1;
            }
            return result;
        }
    }

    TEST(HttpStreaming, NegotiationEventsHeartbeatsDisconnectsAndShutdownUseProductionControllers) {
        using namespace std::chrono_literals;
        LogCapture logs;
        TestFiles files;
        trantor::EventLoopThread backendLoop("stream backend"), clientLoop("stream client");
        backendLoop.run();
        clientLoop.run();
        const auto shutdownCancelled = std::make_shared<std::promise<void>>();
        auto shutdownCancellationReady = shutdownCancelled->get_future();
        std::shared_ptr<std::stop_callback<std::function<void()>>> shutdownCancellation;
        const std::map<std::string, std::shared_ptr<DisconnectProbe>> disconnects{
            {"disconnect-preparation", std::make_shared<DisconnectProbe>()},
            {"disconnect-queue", std::make_shared<DisconnectProbe>()},
            {"disconnect-progress", std::make_shared<DisconnectProbe>()}
        };
        auto implementation = std::make_shared<TestArchitecture>();
        implementation->onPronunciation = [&](arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> complete) {
            const auto mode = input.context.singer.id;
            const auto progress = input.progress;
            const auto success = [complete] { complete(arch::PronunciationOutput{{{"line\ntext", {"line\ntext"}}}}); };
            if (mode == "immediate-error") {
                complete(fail<arch::SynthesisErrors>(arch::InvalidInputError("Invalid test input")));
            } else if (mode == "dispatch-error") {
                throw std::runtime_error("Dispatch failed");
            } else if (mode == "heartbeat") {
                progress.queue();
                backendLoop.getLoop()->runAfter(1.3, [progress] { progress.advance(0); progress.advance(0.25); });
                backendLoop.getLoop()->runAfter(2.5, success);
            } else if (mode == "preparation") {
                backendLoop.getLoop()->runAfter(1.2, success);
            } else if (const auto it = disconnects.find(mode); it != disconnects.end()) {
                const auto weak = std::weak_ptr<DisconnectProbe>(it->second);
                it->second->cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(stop, [weak, complete, progress, loop = backendLoop.getLoop()] {
                    if (const auto probe = weak.lock()) probe->cancelled.set_value();
                    loop->queueInLoop([weak, complete, progress] {
                        progress.advance(0.75);
                        complete(fail<arch::SynthesisErrors>(arch::CancelledError("Disconnected test request")));
                        complete(arch::PronunciationOutput{});
                        if (const auto probe = weak.lock()) probe->completed.set_value();
                    });
                });
                if (mode != "disconnect-preparation") progress.queue();
                if (mode == "disconnect-progress") {
                    progress.advance(0);
                    progress.advance(0.25);
                }
            } else if (mode == "shutdown") {
                shutdownCancellation = std::make_shared<std::stop_callback<std::function<void()>>>(stop, [shutdownCancelled, complete, progress, loop = backendLoop.getLoop()] {
                    shutdownCancelled->set_value();
                    loop->queueInLoop([complete, progress] {
                        progress.advance(0.5);
                        complete(fail<arch::SynthesisErrors>(arch::CancelledError("Stopped test request")));
                    });
                });
                progress.queue();
            } else if (mode == "late-error") {
                progress.queue();
                progress.advance(0);
                progress.advance(1);
                complete(fail<arch::SynthesisErrors>(arch::OperationError("Result encoding failed")));
            } else {
                progress.queue();
                progress.queue();
                progress.advance(0);
                progress.advance(0.4);
                progress.queue();
                progress.advance(0.2);
                progress.advance(std::numeric_limits<double>::quiet_NaN());
                progress.advance(-1);
                progress.advance(2);
                progress.advance(0.9);
                progress.advance(1);
                success();
                success();
                progress.advance(1);
            }
        };

        const auto errorRoot = std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization" / "errors";
        const localization::Catalog::Source sources[]{
            {"http", errorRoot / "http"},
            {"api_v1", errorRoot / "api_v1"},
        };
        const auto translations = localization::Catalog::load(sources, "en_US");
        ASSERT_TRUE(translations);
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>(*translations);
        auto access = std::make_shared<http::AccessLog>();
        auto &app = drogon::app();
        app.setThreadNum(1);
        app.setUploadPath(stdc::path::to_utf8(files.path() / "http-cache"));
        app.setDocumentRoot(stdc::path::to_utf8(files.path()));
        access->install(app);
        api_v1::registerControllers(app, lifetime, testCatalog({implementation}), {}, 1s);
        app.setConnectionCallback([lifetime, access](const auto &connection) {
            access->connectionChanged(connection);
            lifetime->connectionChanged(connection);
        });
        std::promise<std::uint16_t> portPromise;
        auto portFuture = portPromise.get_future();
        app.addListener("127.0.0.1", 0);
        app.registerBeginningAdvice([&] { portPromise.set_value(app.getListeners().front().toPort()); });
        StreamingServer server(app, access, lifetime);
        ASSERT_EQ(portFuture.wait_for(10s), std::future_status::ready);
        const auto port = portFuture.get();

        const auto readyPromise = std::make_shared<std::promise<void>>();
        auto ready = readyPromise->get_future();
        auto readiness = std::make_shared<trantor::TcpClient>(clientLoop.getLoop(), trantor::InetAddress("127.0.0.1", port), "stream readiness");
        readiness->setConnectionCallback([readyPromise](const auto &connection) { if (connection->connected()) readyPromise->set_value(); });
        readiness->enableRetry();
        readiness->connect();
        const auto readyStatus = ready.wait_for(5s);
        readiness->stop();
        readiness->disconnect();
        ASSERT_EQ(readyStatus, std::future_status::ready);

        auto client = drogon::HttpClient::newHttpClient("127.0.0.1", port, false, clientLoop.getLoop());
        const auto request = [&](std::string payload, std::string accept = "application/x-ndjson", std::string path = "/v1/synth/pronunciation", std::string contentType = "application/json", std::string acceptLanguage = {}) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Post);
            request->setPath(path);
            request->addHeader("accept", accept);
            if (!acceptLanguage.empty()) request->addHeader("accept-language", acceptLanguage);
            request->setContentTypeString(contentType);
            request->setBody(std::move(payload));
            auto promise = std::make_shared<std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>>>();
            auto future = promise->get_future();
            client->sendRequest(request, [promise](auto result, const auto &response) { promise->set_value({result, response}); }, 10);
            const auto status = future.wait_for(12s);
            EXPECT_EQ(status, std::future_status::ready);
            if (status != std::future_status::ready) return drogon::HttpResponsePtr{};
            auto [result, response] = future.get();
            EXPECT_EQ(result, drogon::ReqResult::Ok);
            return response;
        };

        const auto success = request(body("success"));
        ASSERT_TRUE(success);
        const auto sequence = events(*success);
        ASSERT_EQ(sequence.size(), 6U) << success->body();
        EXPECT_EQ(sequence[0]["event"], "queue");
        EXPECT_FALSE(sequence[0].isMember("data"));
        for (std::size_t i = 0; const auto value : {0.0, 0.4, 0.9, 1.0}) {
            EXPECT_EQ(sequence[++i]["event"], "progress");
            EXPECT_DOUBLE_EQ(sequence[i]["data"]["value"].asDouble(), value);
        }
        EXPECT_EQ(sequence.back()["event"], "result");
        EXPECT_EQ(sequence.back()["data"]["output"]["notes"][0]["pronunciation"], "line\ntext");

        for (const auto &[payload, expectedStatus] : {std::pair{std::string("{"), 400}, std::pair{std::string("{}"), 400}, std::pair{body("immediate-error"), 422}, std::pair{body("dispatch-error"), 500}}) {
            const auto response = request(payload);
            ASSERT_TRUE(response);
            EXPECT_EQ(response->statusCode(), drogon::k200OK);
            const auto sequence = events(*response);
            ASSERT_EQ(sequence.size(), 1U);
            EXPECT_EQ(sequence.front()["event"], "error");
            EXPECT_EQ(sequence.front()["data"]["status"].asInt(), expectedStatus);
        }
        const auto unsupportedPayload = request(body("success"), "application/x-ndjson", "/v1/synth/pronunciation", "text/plain");
        ASSERT_TRUE(unsupportedPayload);
        const auto payloadEvents = events(*unsupportedPayload);
        ASSERT_EQ(payloadEvents.size(), 1U);
        EXPECT_EQ(payloadEvents[0]["data"]["status"].asInt(), 415);
        auto unknown = body("success");
        unknown.replace(unknown.find("diffsinger"), std::string("diffsinger").size(), "unknown");
        const auto unknownResponse = request(unknown, "application/x-ndjson", "/v1/synth/pronunciation", "application/json", "zh-CN");
        ASSERT_TRUE(unknownResponse);
        const auto unknownEvents = events(*unknownResponse);
        ASSERT_EQ(unknownEvents.size(), 1U);
        EXPECT_EQ(unknownEvents[0]["event"], "error");
        EXPECT_EQ(unknownEvents[0]["data"]["status"].asInt(), 422);
        EXPECT_EQ(unknownEvents[0]["data"]["title"].asString(), "输入无效");
        EXPECT_EQ(unknownEvents[0]["data"]["detail"].asString(), "不支持架构“unknown”");

        for (const auto path : {"/v1/synth/phoneme", "/v1/synth/duration", "/v1/synth/parameter", "/v1/synth/audio", "/v1/extract/note", "/v1/extract/pitch", "/v1/extract/tempo", "/v1/extract/separation"}) {
            const auto response = request("{", "application/x-ndjson", path);
            ASSERT_TRUE(response) << path;
            const auto sequence = events(*response);
            ASSERT_EQ(sequence.size(), 1U) << path;
            EXPECT_EQ(sequence.front()["event"], "error") << path;
        }

        const auto late = request(body("late-error"));
        ASSERT_TRUE(late);
        const auto lateEvents = events(*late);
        ASSERT_EQ(lateEvents.size(), 4U);
        EXPECT_DOUBLE_EQ(lateEvents[2]["data"]["value"].asDouble(), 1);
        EXPECT_EQ(lateEvents.back()["event"], "error");

        const auto heartbeat = request(body("heartbeat"));
        ASSERT_TRUE(heartbeat);
        const auto heartbeatEvents = events(*heartbeat);
        EXPECT_GE(std::ranges::count_if(heartbeatEvents, [](const auto &event) { return event["event"] == "queue"; }), 2);
        EXPECT_GE(std::ranges::count_if(heartbeatEvents, [](const auto &event) { return event["event"] == "progress" && event["data"]["value"].asDouble() == 0.25; }), 2);
        ASSERT_FALSE(heartbeatEvents.empty());
        EXPECT_EQ(heartbeatEvents.back()["event"], "result");
        const auto preparation = request(body("preparation"));
        ASSERT_TRUE(preparation);
        const auto preparationEvents = events(*preparation);
        ASSERT_EQ(preparationEvents.size(), 1U);
        EXPECT_EQ(preparationEvents.front()["event"], "result");

        const auto json = request(body("success"), "application/json;q=1, application/x-ndjson;q=0.5");
        ASSERT_TRUE(json);
        EXPECT_EQ(json->statusCode(), drogon::k200OK);
        ASSERT_TRUE(json->getJsonObject());
        EXPECT_TRUE(json->getJsonObject()->isMember("output"));
        const auto rejected = request(body("success"), "text/plain");
        ASSERT_TRUE(rejected);
        EXPECT_EQ(rejected->statusCode(), drogon::k406NotAcceptable);

        for (const auto &[mode, probe] : disconnects) {
            SCOPED_TRACE(mode);
            auto cancelled = probe->cancelled.get_future();
            auto completed = probe->completed.get_future();
            auto received = std::make_shared<std::string>();
            auto receivedPromise = std::make_shared<std::promise<trantor::TcpConnectionPtr>>();
            auto receivedReady = receivedPromise->get_future();
            auto seen = std::make_shared<std::atomic_flag>();
            seen->clear();
            const std::string marker = mode == "disconnect-preparation" ? "\r\n\r\n" : mode == "disconnect-queue" ? "\"event\":\"queue\"" : "\"value\":0.25";
            auto disconnectClient = std::make_shared<trantor::TcpClient>(clientLoop.getLoop(), trantor::InetAddress("127.0.0.1", port), mode);
            disconnectClient->setConnectionCallback([payload = body(mode)](const auto &connection) {
                if (connection->connected()) connection->send(std::format("POST /v1/synth/pronunciation HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAccept: application/x-ndjson\r\nContent-Length: {}\r\n\r\n{}", payload.size(), payload));
            });
            disconnectClient->setMessageCallback([received, receivedPromise, seen, marker](const auto &connection, trantor::MsgBuffer *buffer) {
                received->append(buffer->peek(), buffer->readableBytes());
                buffer->retrieveAll();
                if (received->find(marker) != received->npos && !seen->test_and_set()) receivedPromise->set_value(connection);
            });
            disconnectClient->connect();
            ASSERT_EQ(receivedReady.wait_for(5s), std::future_status::ready);
            disconnectClient->stop();
            receivedReady.get()->forceClose();
            ASSERT_EQ(cancelled.wait_for(5s), std::future_status::ready);
            ASSERT_EQ(completed.wait_for(5s), std::future_status::ready);
            probe->cancellation.reset();
            const auto followup = request(body("success"));
            ASSERT_TRUE(followup);
            const auto followupEvents = events(*followup);
            ASSERT_FALSE(followupEvents.empty());
            EXPECT_EQ(followupEvents.back()["event"], "result");
        }
        const auto accessRecords = logs.accessRecords();
        EXPECT_EQ(std::ranges::count_if(accessRecords, [](const auto &record) { return record.message.contains("outcome=client_disconnected"); }), 3);
        EXPECT_TRUE(logs.records("dssp.http.TaskStream", stdc::Logger::Critical).empty());

        auto shutdownReadyPromise = std::make_shared<std::promise<void>>();
        auto shutdownReady = shutdownReadyPromise->get_future();
        auto shutdownReceived = std::make_shared<std::string>();
        auto shutdownSeen = std::make_shared<std::atomic_flag>();
        shutdownSeen->clear();
        auto shutdownClient = std::make_shared<trantor::TcpClient>(clientLoop.getLoop(), trantor::InetAddress("127.0.0.1", port), "stream shutdown");
        shutdownClient->setConnectionCallback([payload = body("shutdown")](const auto &connection) {
            if (connection->connected()) connection->send(std::format("POST /v1/synth/pronunciation HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAccept: application/x-ndjson\r\nContent-Length: {}\r\n\r\n{}", payload.size(), payload));
        });
        shutdownClient->setMessageCallback([shutdownReceived, shutdownSeen, shutdownReadyPromise](const auto &, trantor::MsgBuffer *buffer) {
            shutdownReceived->append(buffer->peek(), buffer->readableBytes());
            buffer->retrieveAll();
            if (shutdownReceived->find("\"event\":\"queue\"") != shutdownReceived->npos && !shutdownSeen->test_and_set()) shutdownReadyPromise->set_value();
        });
        shutdownClient->connect();
        ASSERT_EQ(shutdownReady.wait_for(5s), std::future_status::ready);
        lifetime->cancelAll();
        EXPECT_EQ(shutdownCancellationReady.wait_for(5s), std::future_status::ready);
        shutdownClient->stop();
        shutdownClient->disconnect();
        shutdownCancellation.reset();
    }
}
