#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include <dsspapiv1/DiscoveryController.h>

namespace dssp::tests {
    TEST(Discovery, CatalogLinksTheApiToItsSpecification) {
        api_v1::DiscoveryController controller;
        drogon::HttpResponsePtr response;
        controller.catalog(drogon::HttpRequest::newHttpRequest(), [&](const auto &value) { response = value; });
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ(response->contentTypeString(), "application/linkset+json; profile=\"https://www.rfc-editor.org/info/rfc9727\"");
        EXPECT_EQ(response->getHeader("link"), "</.well-known/api-catalog>; rel=\"api-catalog\"; type=\"application/linkset+json\"");
        const auto body = response->getJsonObject();
        ASSERT_NE(body, nullptr);
        EXPECT_EQ(body->size(), 1U);
        const auto &links = (*body)["linkset"];
        ASSERT_TRUE(links.isArray());
        ASSERT_EQ(links.size(), 2U);
        EXPECT_EQ(links[0]["anchor"].asString(), "/.well-known/api-catalog");
        ASSERT_TRUE(links[0]["item"].isArray());
        ASSERT_EQ(links[0]["item"].size(), 1U);
        EXPECT_EQ(links[0]["item"][0]["href"].asString(), "/v1");
        EXPECT_EQ(links[1]["anchor"], links[0]["item"][0]["href"]);
        ASSERT_TRUE(links[1]["service-desc"].isArray());
        ASSERT_EQ(links[1]["service-desc"].size(), 1U);
        EXPECT_EQ(links[1]["service-desc"][0]["href"].asString(), "/v1/openapi.json");
        EXPECT_EQ(links[1]["service-desc"][0]["type"].asString(), "application/json");
    }

    TEST(Discovery, SpecificationPreservesTheSourceBytes) {
        std::ifstream source(std::filesystem::path(u8"" DSSP_TEST_OPENAPI_FILE), std::ios::binary);
        ASSERT_TRUE(source);
        const std::string expected{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        ASSERT_FALSE(expected.empty());

        api_v1::DiscoveryController controller;
        drogon::HttpResponsePtr response;
        controller.specification(drogon::HttpRequest::newHttpRequest(), [&](const auto &value) { response = value; });
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), drogon::k200OK);
        EXPECT_EQ(response->contentTypeString(), "application/json");
        EXPECT_EQ(response->body(), expected);
        const auto body = response->getJsonObject();
        ASSERT_NE(body, nullptr);
        EXPECT_TRUE((*body)["paths"].isMember("/info"));
        EXPECT_FALSE((*body)["paths"].isMember("/.well-known/api-catalog"));
        EXPECT_FALSE((*body)["paths"].isMember("/openapi.json"));
    }
}
