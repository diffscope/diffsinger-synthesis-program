#include <optional>

#include <gtest/gtest.h>

#include <dssphttp/RequestLifetime.h>

namespace dssp::tests {

    TEST(RequestLifetime, ShutdownCancelsActiveAndFutureRequests) {
        auto registry = std::make_shared<http::RequestLifetimeRegistry>();
        auto request = drogon::HttpRequest::newHttpRequest();
        request->addHeader("accept-language", "zh-CN,en;q=0.5");
        const auto active = registry->track(*request);
        EXPECT_FALSE(active.stopRequested());
        EXPECT_EQ(active.responseContext().locale.acceptLanguage(), "zh-CN,en;q=0.5");
        registry->cancelAll();
        EXPECT_TRUE(active.stopRequested());
        EXPECT_TRUE(registry->track(*request).stopRequested());
    }

    TEST(RequestLifetime, CompletionReleasesRegistrationAndCancellationDoesNotLeak) {
        auto registry = std::make_shared<http::RequestLifetimeRegistry>();
        auto request = drogon::HttpRequest::newHttpRequest();
        std::stop_token completed;
        {
            const auto context = registry->track(*request);
            completed = context.cancellation();
        }

        const auto next = registry->track(*request);
        registry->cancelAll();
        EXPECT_FALSE(completed.stop_requested());
        EXPECT_TRUE(next.stopRequested());
    }

    TEST(RequestLifetime, StopCallbacksMayReleaseTheirOwnRequestWithoutDeadlock) {
        auto registry = std::make_shared<http::RequestLifetimeRegistry>();
        auto request = drogon::HttpRequest::newHttpRequest();
        std::optional<http::RequestContext> context = registry->track(*request);
        bool called = false;
        std::stop_callback callback(context->cancellation(), [&] {
            called = true;
            context.reset();
        });
        registry->cancelAll();
        EXPECT_TRUE(called);
        EXPECT_FALSE(context);
    }

}
