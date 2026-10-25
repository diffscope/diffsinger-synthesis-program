#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <dsspapiv1/Controllers.h>
#include <dssptests/TestArchitecture.h>
#include <dssptests/TestCoroutine.h>

namespace dssp::tests {
    drogon::HttpRequestPtr jsonRequest(std::string body) {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Post);
        request->addHeader("content-type", "application/json");
        request->setBody(std::move(body));
        return request;
    }

    TEST(Controller, InfoReturnsTheFixedMajorAndMinorApiVersion) {
        api_v1::ApplicationController application;
        drogon::HttpResponsePtr response;
        application.info(drogon::HttpRequest::newHttpRequest(), [&](const auto &r) { response = r; });
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        const auto body = response->getJsonObject();
        ASSERT_NE(body, nullptr);
        ASSERT_TRUE((*body)["dssp"].isObject());
        const auto &version = (*body)["dssp"]["apiVersion"];
        ASSERT_TRUE(version.isObject());
        ASSERT_TRUE(version["major"].isNumeric());
        ASSERT_TRUE(version["minor"].isNumeric());
        EXPECT_EQ(version.size(), 2U);
        EXPECT_DOUBLE_EQ(version["major"].asDouble(), 1.0);
        EXPECT_DOUBLE_EQ(version["minor"].asDouble(), 0.0);
    }

    TEST(Controller, EnvTagDoesNotUseRequestCancellation) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        api_v1::SynthesisController controller(lifetime, testCatalog());
        lifetime->cancelAll();
        drogon::HttpResponsePtr response;
        controller.envTag(jsonRequest(R"({"context":{"arch":"diffsinger","archExtra":null,"singers":[{"id":"unknown","extra":null}]}})"), [&](const auto &r) { response = r; });
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*response->getJsonObject())["envTag"].asString(), "");
    }

    TEST(Controller, InjectedArchitectureDetailsCanReturnNull) {
        api_v1::CatalogController catalog(testCatalog());
        drogon::HttpResponsePtr response;
        catalog.singer(drogon::HttpRequest::newHttpRequest(), [&](const auto &r) { response = r; }, "diffsinger", "unknown");
        ASSERT_NE(response, nullptr);
        ASSERT_NE(response->getJsonObject(), nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_TRUE(response->getJsonObject()->isNull());
        catalog.group(drogon::HttpRequest::newHttpRequest(), [&](const auto &r) { response = r; }, "diffsinger", "unknown");
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_TRUE(response->getJsonObject()->isNull());
    }

    TEST(Controller, ArchitectureCatalogListsTheInjectedImplementation) {
        api_v1::CatalogController controller(testCatalog());
        const auto request = drogon::HttpRequest::newHttpRequest();
        drogon::HttpResponsePtr response;
        controller.architectures(request, [&](const auto &r) { response = r; });
        ASSERT_NE(response, nullptr);
        const auto list = response->getJsonObject();
        ASSERT_NE(list, nullptr);
        ASSERT_TRUE(list->isArray());
        ASSERT_EQ(list->size(), 1U);
        EXPECT_EQ((*list)[0]["id"].asString(), "diffsinger");
        EXPECT_TRUE((*list)[0]["parameters"].isObject());
        controller.architecture(request, [&](const auto &r) { response = r; }, "diffsinger");
        EXPECT_EQ((*response->getJsonObject())["id"].asString(), "diffsinger");
        controller.singers(request, [&](const auto &r) { response = r; });
        EXPECT_TRUE(response->getJsonObject()->isArray());
        EXPECT_TRUE(response->getJsonObject()->empty());
    }

    TEST(Controller, UnknownArchitectureHasCatalogAndProcessStatuses) {
        api_v1::CatalogController catalog(testCatalog());
        drogon::HttpResponsePtr response;
        catalog.architecture(drogon::HttpRequest::newHttpRequest(), [&](const auto &r) { response = r; }, "future");
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
        EXPECT_EQ(response->contentTypeString(), "application/problem+json");
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        api_v1::SynthesisController synthesis(lifetime, testCatalog());
        synthesis.envTag(jsonRequest(R"({"context":{"arch":"future","archExtra":null,"singers":[{"id":"unknown","extra":null}]}})"), [&](const auto &r) { response = r; });
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_EQ((*response->getJsonObject())["type"].asString(), "urn:dssp:error:invalid-input");
        EXPECT_FALSE(response->getJsonObject()->isMember("errors"));
        startCoroutine(synthesis.pronunciation(jsonRequest(R"({"context":{"arch":"future","archExtra":null,"singer":{"id":"unknown","extra":null}},"input":{"notes":[]}})"), [&](const auto &r) { response = r; }));
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_EQ((*response->getJsonObject())["type"].asString(), "urn:dssp:error:invalid-input");
        EXPECT_NE((*response->getJsonObject())["detail"].asString().find("future"), std::string::npos);
    }

    TEST(Controller, EachControllerUsesItsInjectedSetAndPreservesItsOrder) {
        api_v1::CatalogController first(testCatalog({std::make_shared<TestArchitecture>("alpha"), std::make_shared<TestArchitecture>("beta")}));
        api_v1::CatalogController second(testCatalog({std::make_shared<TestArchitecture>("gamma")}));
        const auto request = drogon::HttpRequest::newHttpRequest();
        drogon::HttpResponsePtr response;
        first.architectures(request, [&](const auto &value) { response = value; });
        ASSERT_NE(response, nullptr);
        const auto body = response->getJsonObject();
        ASSERT_NE(body, nullptr);
        ASSERT_EQ(body->size(), 2U);
        EXPECT_EQ((*body)[0]["id"].asString(), "alpha");
        EXPECT_EQ((*body)[1]["id"].asString(), "beta");
        second.architecture(request, [&](const auto &value) { response = value; }, "alpha");
        EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
        second.architecture(request, [&](const auto &value) { response = value; }, "gamma");
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*response->getJsonObject())["id"].asString(), "gamma");
    }

    TEST(Controller, DeferredSynthesisRetainsTheSelectedInstanceAfterControllerDestruction) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        arch::Completion<arch::PronunciationOutput> pending;
        std::weak_ptr<const arch::Architecture> observed;
        drogon::HttpResponsePtr response;
        {
            auto implementation = std::make_shared<TestArchitecture>("injected");
            observed = implementation;
            implementation->onPronunciation = [&](arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> completion) {
                EXPECT_EQ(input.context.singer.id, "singer-A");
                EXPECT_FALSE(stop.stop_requested());
                pending = std::move(completion);
            };
            api_v1::SynthesisController controller(lifetime, testCatalog({implementation}));
            startCoroutine(controller.pronunciation(jsonRequest(R"({"context":{"arch":"injected","archExtra":null,"singer":{"id":"singer-A","extra":null}},"input":{"notes":[]}})"), [&](const auto &value) { response = value; }));
        }

        ASSERT_TRUE(pending);
        EXPECT_EQ(response, nullptr);
        EXPECT_FALSE(observed.expired());
        std::jthread worker([pending] { pending(arch::PronunciationOutput{}); });
        worker.join();
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        pending = {};
        EXPECT_TRUE(observed.expired());
    }

    TEST(Controller, CancellableSynthesisDoesNotStartAfterShutdown) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        api_v1::SynthesisController controller(lifetime, testCatalog());
        lifetime->cancelAll();
        for (const auto handler : {&api_v1::SynthesisController::pronunciation, &api_v1::SynthesisController::phoneme, &api_v1::SynthesisController::duration, &api_v1::SynthesisController::parameter, &api_v1::SynthesisController::audio}) {
            bool replied = false;
            startCoroutine((controller.*handler)(jsonRequest("{}"), [&](const auto &) { replied = true; }));
            EXPECT_FALSE(replied);
        }
    }

    TEST(Controller, CatalogCollectionsAndMediaUseInjectedResults) {
        api_v1::CatalogController controller(testCatalog());
        const auto request = drogon::HttpRequest::newHttpRequest();
        drogon::HttpResponsePtr response;
        for (const auto handler : {&api_v1::CatalogController::archSingers, &api_v1::CatalogController::groups}) {
            response.reset();
            (controller.*handler)(request, [&](const auto &r) { response = r; }, "diffsinger");
            ASSERT_NE(response, nullptr);
            EXPECT_EQ(response->statusCode(), drogon::k200OK);
            ASSERT_NE(response->getJsonObject(), nullptr);
            EXPECT_TRUE(response->getJsonObject()->isArray());
            EXPECT_TRUE(response->getJsonObject()->empty());
        }

        for (const auto handler : {&api_v1::CatalogController::groupSingers, &api_v1::CatalogController::demoAudio}) {
            response.reset();
            (controller.*handler)(request, [&](const auto &r) { response = r; }, "diffsinger", "unknown");
            ASSERT_NE(response, nullptr);
            EXPECT_EQ(response->statusCode(), drogon::k200OK);
            EXPECT_TRUE(response->getJsonObject()->isArray());
            EXPECT_TRUE(response->getJsonObject()->empty());
        }

        controller.background(request, [&](const auto &r) { response = r; }, "diffsinger", "unknown");
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*response->getJsonObject())["backgroundUrl"].asString(), "");
    }

    TEST(Controller, InvalidPayloadsAndFieldsReturn400AndUnsupportedPayloadsReturn415) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        api_v1::SynthesisController controller(lifetime, testCatalog());
        auto status = [&](const drogon::HttpRequestPtr &request) {
            drogon::HttpResponsePtr response;
            startCoroutine(controller.audio(request, [&](const auto &r) { response = r; }));
            return response->statusCode();
        };

        EXPECT_EQ(status(jsonRequest("{")), drogon::k400BadRequest);
        EXPECT_EQ(status(jsonRequest("")), drogon::k400BadRequest);
        EXPECT_EQ(status(jsonRequest("{}")), drogon::k400BadRequest);
        EXPECT_EQ(status(jsonRequest("null")), drogon::k400BadRequest);
        EXPECT_EQ(status(jsonRequest("{/*comment*/}")), drogon::k400BadRequest);
        EXPECT_EQ(status(jsonRequest(std::string(140, '[') + "0" + std::string(140, ']'))), drogon::k400BadRequest);
        auto unsupported = jsonRequest("{}");
        unsupported->addHeader("content-type", "text/plain");
        EXPECT_EQ(status(unsupported), drogon::k415UnsupportedMediaType);
        unsupported = jsonRequest("{}");
        unsupported->addHeader("content-encoding", "gzip");
        EXPECT_EQ(status(unsupported), drogon::k415UnsupportedMediaType);
        auto parameters = jsonRequest("{}");
        parameters->addHeader("content-type", "Application/JSON; charset=\"utf-8\"");
        EXPECT_EQ(status(parameters), drogon::k400BadRequest);
        parameters->addHeader("content-type", "application/json; charset=\"unterminated");
        EXPECT_EQ(status(parameters), drogon::k415UnsupportedMediaType);
    }

    TEST(Controller, ExtractionDistinguishesRequestValidationFromBusinessRejections) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        const auto environment = inference::Environment::create({});
        ASSERT_TRUE(environment);
        const auto service = extraction::Service::create(*environment);
        ASSERT_TRUE(service);
        api_v1::ExtractionController controller(lifetime, *service);
        drogon::HttpResponsePtr response;
        for (const auto body : {"{}", R"({"extractor":"unknown","input":{"audioUrl":7}})", R"({"extractor":"unknown","input":{"audioUrl":"relative.wav"}})"}) {
            startCoroutine(controller.note(jsonRequest(body), [&](const auto &r) { response = r; }));
            ASSERT_NE(response, nullptr);
            EXPECT_EQ(response->statusCode(), drogon::k400BadRequest);
            EXPECT_EQ((*response->getJsonObject())["status"].asInt(), 400);
            EXPECT_TRUE(response->getJsonObject()->isMember("errors"));
        }
        auto request = jsonRequest(R"({"extractor":"unknown","input":{"audioUrl":"https://example.com/a.flac"}})");
        startCoroutine(controller.note(request, [&](const auto &r) { response = r; }));
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_FALSE(response->getJsonObject()->isMember("errors"));
        startCoroutine(controller.tempo(request, [&](const auto &r) { response = r; }));
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        startCoroutine(controller.separation(request, [&](const auto &r) { response = r; }));
        EXPECT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        controller.extractors(request, [&](const auto &r) { response = r; });
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        for (const auto key : {"note", "pitch", "tempo", "separation"}) EXPECT_TRUE((*response->getJsonObject())[key].empty());
        controller.noteExtractor(request, [&](const auto &r) { response = r; }, "unknown");
        EXPECT_EQ(response->statusCode(), drogon::k404NotFound);
        (*service)->shutdown();
        (*environment)->shutdown();
    }

    TEST(Controller, ValidSynthesisRequestsReturnEmptyStageOutputs) {
        auto lifetime = std::make_shared<http::RequestLifetimeRegistry>();
        api_v1::SynthesisController controller(lifetime, testCatalog());
        const auto single = R"({"arch":"diffsinger","archExtra":null,"singer":{"id":"unknown","extra":null}})";
        const auto multi = R"({"arch":"diffsinger","archExtra":null,"singers":[{"id":"unknown","extra":null}]})";
        const auto notes = R"({"notes":[]})";
        const auto piece = R"({"pieceDuration":0,"notes":[],"mix":[],"mixSampleRate":100,"parameters":{}})";
        auto call = [&](auto handler, const char *context, const char *input) {
            std::string body = std::string(R"({"context":)") + context;
            if (input) {
                body += std::string(R"(,"input":)") + input;
            }
            body += '}';
            drogon::HttpResponsePtr response;
            if constexpr (std::is_void_v<std::invoke_result_t<decltype(handler), const api_v1::SynthesisController &, drogon::HttpRequestPtr, http::Callback>>) {
                (controller.*handler)(jsonRequest(std::move(body)), [&](const auto &value) { response = value; });
            } else {
                startCoroutine((controller.*handler)(jsonRequest(std::move(body)), [&](const auto &value) { response = value; }));
            }
            EXPECT_NE(response, nullptr);
            return response;
        };

        const auto envTag = call(&api_v1::SynthesisController::envTag, multi, nullptr);
        ASSERT_NE(envTag, nullptr);
        EXPECT_EQ((*envTag->getJsonObject())["envTag"].asString(), "");
        for (const auto handler : {&api_v1::SynthesisController::pronunciation, &api_v1::SynthesisController::phoneme, &api_v1::SynthesisController::duration}) {
            const bool duration = handler == &api_v1::SynthesisController::duration;
            const auto response = call(handler, duration ? multi : single, duration ? piece : notes);
            ASSERT_NE(response, nullptr);
            EXPECT_EQ(response->statusCode(), drogon::k200OK);
            EXPECT_TRUE((*response->getJsonObject())["output"]["notes"].isArray());
            EXPECT_TRUE((*response->getJsonObject())["output"]["notes"].empty());
        }

        const auto parameters = call(&api_v1::SynthesisController::parameter, multi, piece);
        ASSERT_NE(parameters, nullptr);
        EXPECT_EQ(parameters->statusCode(), drogon::k200OK);
        EXPECT_TRUE((*parameters->getJsonObject())["output"]["parameters"].isObject());
        const auto audio = call(&api_v1::SynthesisController::audio, multi, piece);
        ASSERT_NE(audio, nullptr);
        EXPECT_EQ(audio->statusCode(), drogon::k200OK);
        EXPECT_EQ((*audio->getJsonObject())["output"]["audioUrl"].asString(), "");
    }

    TEST(Controller, MediaQueryChecksSyntaxWithoutPerformingNegotiation) {
        api_v1::CatalogController controller(testCatalog());
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setParameter("acceptableFormats", "audio/flac");
        request->setParameter("acceptableSchemes", "https");
        drogon::HttpResponsePtr response;
        controller.avatar(request, [&](const auto &r) { response = r; }, "diffsinger", "unknown");
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ((*response->getJsonObject())["avatarUrl"].asString(), "");
        request->setParameter("acceptableFormats", "not-a-media-type");
        controller.avatar(request, [&](const auto &r) { response = r; }, "diffsinger", "unknown");
        EXPECT_EQ(response->statusCode(), drogon::k400BadRequest);
        for (const auto invalid : {"audio/wav;codecs=pcm", "audio/*", "*/*"}) {
            request->setParameter("acceptableFormats", invalid);
            controller.avatar(request, [&](const auto &r) { response = r; }, "diffsinger", "unknown");
            EXPECT_EQ(response->statusCode(), drogon::k400BadRequest);
            const auto body = response->getJsonObject();
            ASSERT_NE(body, nullptr);
            EXPECT_EQ((*body)["errors"][0]["pointer"].asString(), "/acceptableFormats/0");
            EXPECT_EQ((*body)["errors"][0]["errorType"].asString(), "format");
        }
    }

}
