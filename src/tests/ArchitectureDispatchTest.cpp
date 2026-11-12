#include <atomic>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dsspapiv1/ArchitectureDispatch.h>
#include <dssptests/LogCapture.h>
#include <dssptests/TestArchitecture.h>
#include <dssptests/TestCoroutine.h>

namespace dssp::tests {
    namespace {
        drogon::HttpRequestPtr pronunciationRequest() {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Post);
            request->addHeader("content-type", "application/json");
            request->setBody(R"({"context":{"arch":"diffsinger","archExtra":{"preset":7},"singer":{"id":"singer-A","extra":{"speaker":"lead"}}},"input":{"notes":[{"lyric":"la","language":"eng"}]}})");
            return request;
        }
    }

    TEST(ArchitectureDispatch, DeferredCompletionOwnsInputsAndCanReplyFromAnotherThread) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        arch::Completion<arch::PronunciationOutput> pending;
        drogon::HttpResponsePtr response;
        startCoroutine(api_v1::detail::process<api_v1::dto::PronunciationRequest, api_v1::dto::PronunciationResponse, arch::PronunciationOutput>(
            pronunciationRequest(),
            [&](const auto &r) { response = r; },
            lifetime,
            testCatalog(),
            [&](const arch::Architecture &implementation, arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> completion) {
                EXPECT_EQ(implementation.id(), "diffsinger");
                EXPECT_EQ(input.context.singer.id, "singer-A");
                EXPECT_EQ(input.context.singer.extra["speaker"].asString(), "lead");
                EXPECT_EQ(input.context.archExtra["preset"].asInt(), 7);
                ASSERT_EQ(input.notes.size(), 1U);
                EXPECT_EQ(input.notes[0].language, "eng");
                EXPECT_FALSE(stop.stop_requested());
                pending = std::move(completion);
            }));
        ASSERT_TRUE(pending);
        EXPECT_EQ(response, nullptr);
        std::jthread worker([pending] { pending(arch::PronunciationOutput{{{"la", {"la", "lah"}}}}); });
        worker.join();
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*response->getJsonObject())["output"]["notes"][0]["pronunciation"].asString(), "la");
    }

    TEST(ArchitectureDispatch, RequestContextIsRetainedUntilDeferredCompletion) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        arch::Completion<arch::PronunciationOutput> pending;
        std::stop_token cancellation;
        bool replied = false;
        startCoroutine(api_v1::detail::process<api_v1::dto::PronunciationRequest, api_v1::dto::PronunciationResponse, arch::PronunciationOutput>(
            pronunciationRequest(),
            [&](const auto &) { replied = true; },
            lifetime,
            testCatalog(),
            [&](const arch::Architecture &, arch::PronunciationInput, std::stop_token stop, arch::Completion<arch::PronunciationOutput> completion) {
                cancellation = stop;
                pending = std::move(completion);
            }));
        ASSERT_TRUE(pending);
        EXPECT_FALSE(cancellation.stop_requested());
        lifetime->cancelAll();
        EXPECT_TRUE(cancellation.stop_requested());
        pending(arch::PronunciationOutput{});
        EXPECT_FALSE(replied);
    }

    TEST(ArchitectureDispatch, DroppingCompletedCallbackReleasesItsCancellationRegistration) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        arch::Completion<arch::PronunciationOutput> pending;
        std::stop_token stop;
        startCoroutine(api_v1::detail::process<api_v1::dto::PronunciationRequest, api_v1::dto::PronunciationResponse, arch::PronunciationOutput>(
            pronunciationRequest(),
            [](const auto &) {},
            lifetime,
            testCatalog(),
            [&](const arch::Architecture &, arch::PronunciationInput, std::stop_token token, arch::Completion<arch::PronunciationOutput> completion) {
                stop = token;
                pending = std::move(completion);
            }));
        ASSERT_TRUE(pending);
        pending(arch::PronunciationOutput{});
        pending = {};
        lifetime->cancelAll();
        EXPECT_FALSE(stop.stop_requested());
    }

    TEST(ArchitectureDispatch, MediaQueryForwardsLanguageAndPreferencesWithoutNegotiation) {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->addHeader("accept-language", "zh-CN,en;q=0.5");
        request->setParameter("acceptableFormats", "Audio/FLAC,audio/wav");
        request->setParameter("acceptableSchemes", "https,data");
        drogon::HttpResponsePtr response;
        api_v1::detail::catalogMedia(
            *testCatalog(),
            request,
            [&](const auto &r) { response = r; },
            "diffsinger",
            "singer-A",
            [](const arch::Architecture &implementation, const arch::SingerMediaQuery &query) -> arch::CatalogResult<std::string> {
                EXPECT_EQ(implementation.id(), "diffsinger");
                EXPECT_EQ(query.singer.id, "singer-A");
                EXPECT_EQ(query.singer.display.acceptLanguage, "zh-CN,en;q=0.5");
                EXPECT_EQ(query.media.acceptableFormats, (std::vector<std::string>{"Audio/FLAC", "audio/wav"}));
                EXPECT_EQ(query.media.acceptableSchemes, (std::vector<std::string>{"https", "data"}));
                return std::string{};
            },
            http::RequestLifetimeRegistry{}.responseContext(*request),
            [](std::string url) { return api_v1::dto::SingerAvatarResponse{std::move(url)}; });
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
    }

    TEST(ArchitectureDispatch, DeferredFailurePreservesTheHttpErrorStatus) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        drogon::HttpResponsePtr response;
        const auto request = pronunciationRequest();
        auto complete = api_v1::detail::completion<api_v1::dto::PronunciationResponse, arch::PronunciationOutput>([&](const auto &r) { response = r; }, lifetime->track(*request));
        std::jthread worker([complete] { complete(fail<arch::SynthesisErrors>(arch::InvalidInputError("Singer does not support this language"))); });
        worker.join();
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_EQ((*response->getJsonObject())["detail"].asString(), "Singer does not support this language");
    }

    TEST(ArchitectureDispatch, ConcurrentDuplicateCompletionsSendOnlyOneResponse) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        const auto request = pronunciationRequest();
        std::atomic<unsigned> replies = 0;
        const auto complete = api_v1::detail::completion<api_v1::dto::PronunciationResponse, arch::PronunciationOutput>([&](const auto &) { ++replies; }, lifetime->track(*request));
        std::jthread first([complete] { complete(arch::PronunciationOutput{}); });
        std::jthread second([complete] { complete(arch::PronunciationOutput{}); });
        first.join();
        second.join();
        EXPECT_EQ(replies.load(), 1U);
    }

    TEST(ArchitectureDispatch, ThrowingHttpCallbackIsContainedAtTheAsynchronousLibraryBoundary) {
        LogCapture logs;
        const auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        const auto request = pronunciationRequest();
        unsigned attempts = 0;
        const auto complete = api_v1::detail::completion<api_v1::dto::PronunciationResponse, arch::PronunciationOutput>([&](const auto &) {
            ++attempts;
            throw std::runtime_error("simulated HTTP callback failure");
        }, lifetime->track(*request));
        EXPECT_NO_THROW(complete(arch::PronunciationOutput{}));
        complete(arch::PronunciationOutput{});
        EXPECT_EQ(attempts, 1U);
        const auto errors = logs.records("dssp.api_v1.AsyncReply", stdc::Logger::Critical);
        ASSERT_EQ(errors.size(), 1U);
        EXPECT_NE(errors.front().message.find("simulated HTTP callback failure"), std::string::npos);
    }

}
