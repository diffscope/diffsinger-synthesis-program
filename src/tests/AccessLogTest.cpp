#include <latch>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <json/reader.h>

#include <dssphttp/AccessLog.h>
#include <dssphttp/RequestLifetime.h>
#include <dssphttp/Response.h>
#include <dssptests/LogCapture.h>

namespace dssp::tests {
    TEST(AccessLog, RecordsMethodPathStatusClientAndProcessingTimeAtInfo) {
        LogCapture capture;
        auto log = std::make_shared<http::AccessLog>();
        for (const auto method : {drogon::Get, drogon::Post, drogon::Options}) {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(method);
            request->setPath("/v1/info");
            log->begin(*request);
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k200OK);
            log->finish(*request, *response);
            const auto records = capture.accessRecords();
            ASSERT_FALSE(records.empty());
            const auto &message = records.back().message;
            EXPECT_NE(message.find("method=" + std::string(request->methodString())), std::string::npos);
            EXPECT_NE(message.find("path=\"/v1/info\" status=200"), std::string::npos);
            EXPECT_NE(message.find("duration_ms="), std::string::npos);
            EXPECT_NE(message.find("client="), std::string::npos);
            EXPECT_NE(message.find("outcome=response"), std::string::npos);
        }

        EXPECT_EQ(capture.accessRecords().size(), 3U);
    }

    TEST(AccessLog, StreamCompletionDefersTheRecordAndRetainsTheProblem) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Post);
        request->setPath("/v1/synth/audio");
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        const auto context = http::requestLog(*request);
        context.beginStream();
        log->finish(*request, *drogon::HttpResponse::newHttpResponse());
        EXPECT_TRUE(capture.accessRecords().empty());
        context.problem("operation", "Result encoding failed");
        context.finishStream("/v1/synth/audio");
        context.finishStream("/v1/synth/audio");
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_NE(records.front().message.find("status=200"), std::string::npos);
        EXPECT_NE(records.front().message.find("error=\"operation\""), std::string::npos);
        EXPECT_NE(records.front().message.find("Result encoding failed"), std::string::npos);
    }

    TEST(AccessLog, PreservesHeadWhenDrogonRoutesItAsGet) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Head);
        request->setMethod(drogon::Get);
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        log->finish(*request, *drogon::HttpResponse::newHttpResponse());
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_NE(records.front().message.find("method=HEAD"), std::string::npos);
    }

    TEST(AccessLog, EscapesUntrustedPathsAndExcludesQueryHeadersAndBody) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/v1/unknown\r\nforged-record\x1b");
        request->setParameter("token", "query-secret");
        request->addHeader("authorization", "header-secret");
        request->setBody("body-secret");
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k404NotFound);
        log->finish(*request, *response);
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        const auto &message = records.front().message;
        EXPECT_EQ(message.find_first_of("\r\n\x1b"), std::string::npos);
        EXPECT_NE(message.find("\\r\\nforged-record"), std::string::npos);
        EXPECT_NE(message.find("status=404"), std::string::npos);
        for (const auto secret : {"query-secret", "header-secret", "body-secret"}) {
            EXPECT_EQ(message.find(secret), std::string::npos);
        }
    }

    TEST(AccessLog, ValidationRecordIdentifiesFieldsAndRulesWithoutSubmittedValues) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        auto response = http::problemResponse(http::ValidationError({{"/input/notes", "required", "private rejected value"}}), {http::requestLog(*request)});
        log->finish(*request, *response);
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_NE(records.front().message.find("status=400"), std::string::npos);
        EXPECT_NE(records.front().message.find("error=\"validation\""), std::string::npos);
        EXPECT_NE(records.front().message.find("/input/notes [required]"), std::string::npos);
        EXPECT_EQ(records.front().message.find("private rejected value"), std::string::npos);
    }

    TEST(AccessLog, BoundsPathLengthAndPreservesUtf8AtTheLimit) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        const std::string longPath = "/" + std::string(4096, 'x');
        request->setPath(longPath);
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        log->finish(*request, *drogon::HttpResponse::newHttpResponse());
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_EQ(records.front().message.find(longPath), std::string::npos);
        EXPECT_NE(records.front().message.find("...\" status="), std::string::npos);

        const std::string text = "\xe4\xb8\xad\xe6\x96\x87";
        const auto encoded = http::quoteLogValue(text, 4);
        Json::CharReaderBuilder builder;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value decoded;
        std::string error;
        ASSERT_TRUE(reader->parse(encoded.data(), encoded.data() + encoded.size(), &decoded, &error)) << error;
        EXPECT_EQ(decoded.asString(), "\xe4\xb8\xad...");
        EXPECT_EQ(http::quoteLogValue(std::string_view("public-private").substr(0, 6)), "\"public\"");
        EXPECT_EQ(http::quoteLogValue({}), "\"\"");
    }

    TEST(AccessLog, InternalFailureHasOneCauseRecordWithTheSameRequestContext) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/v1/synthesis/audio");
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        const auto context = http::requestLog(*request);
        const http::RequestErrors error{http::HttpBoundaryError("dependency failed\nmore detail")};
        const auto response = http::problemResponse(error, {context});
        log->finish(*request, *response);
        const auto access = capture.accessRecords();
        const auto failures = capture.records("dssp.http.Response", stdc::Logger::Critical);
        ASSERT_EQ(access.size(), 1U);
        ASSERT_EQ(failures.size(), 1U);
        EXPECT_NE(access.front().message.find(context.description()), std::string::npos);
        EXPECT_NE(failures.front().message.find(context.description()), std::string::npos);
        EXPECT_NE(access.front().message.find("status=500"), std::string::npos);
        EXPECT_NE(failures.front().message.find("dependency failed\\nmore detail"), std::string::npos);
        EXPECT_EQ(failures.front().message.find('\n'), std::string::npos);
    }

    TEST(AccessLog, AsyncContextSharesCorrelationWithoutRetainingTheRequest) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/v1/synthesis/audio");
        const std::weak_ptr<drogon::HttpRequest> weakRequest = request;
        auto log = std::make_shared<http::AccessLog>();
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        log->begin(*request);
        const auto diagnostic = http::requestLog(*request);
        const auto context = lifetime->track(*request);
        EXPECT_NE(context.id(), 0U);
        EXPECT_EQ(context.id(), diagnostic.id());
        request.reset();
        EXPECT_TRUE(weakRequest.expired());
        log->shutdown();
        lifetime->cancelAll();
        EXPECT_TRUE(context.stopRequested());
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_NE(records.front().message.find(diagnostic.description()), std::string::npos);
        EXPECT_NE(records.front().message.find("status=-"), std::string::npos);
        EXPECT_NE(records.front().message.find("outcome=server_shutdown"), std::string::npos);
    }

    TEST(AccessLog, ResponseAndShutdownRaceProducesOneTerminalRecord) {
        LogCapture capture;
        auto request = drogon::HttpRequest::newHttpRequest();
        auto response = drogon::HttpResponse::newHttpResponse();
        auto log = std::make_shared<http::AccessLog>();
        log->begin(*request);
        std::latch ready(3);
        std::jthread complete([&] { ready.arrive_and_wait(); log->finish(*request, *response); });
        std::jthread shutdown([&] { ready.arrive_and_wait(); log->shutdown(); });
        ready.arrive_and_wait();
        complete.join();
        shutdown.join();
        log->finish(*request, *response);
        log->shutdown();
        EXPECT_EQ(capture.accessRecords().size(), 1U);
    }

    TEST(AccessLog, ShutdownDoesNotRelabelCompletedRequestsAndRejectsFutureOnes) {
        LogCapture capture;
        auto log = std::make_shared<http::AccessLog>();
        auto completed = drogon::HttpRequest::newHttpRequest();
        log->begin(*completed);
        log->finish(*completed, *drogon::HttpResponse::newHttpResponse());
        log->shutdown();
        auto next = drogon::HttpRequest::newHttpRequest();
        log->begin(*next);
        log->finish(*next, *drogon::HttpResponse::newHttpResponse());
        const auto records = capture.accessRecords();
        ASSERT_EQ(records.size(), 2U);
        EXPECT_NE(records[0].message.find("outcome=response"), std::string::npos);
        EXPECT_NE(records[1].message.find("outcome=server_shutdown"), std::string::npos);
    }
}
