#include <gtest/gtest.h>

#include <dsspapiv1/ArchitectureErrors.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dssphttp/Response.h>

namespace dssp::tests {

    TEST(Response, ApplicationInfoSerializesBothNumericVersionComponents) {
        const api_v1::dto::ApplicationInfoResponse info{{{3.0, 2.0}}};
        const auto body = api_v1::toJson(info);
        ASSERT_TRUE(body["dssp"].isObject());
        const auto &version = body["dssp"]["apiVersion"];
        ASSERT_TRUE(version.isObject());
        ASSERT_TRUE(version["major"].isNumeric());
        ASSERT_TRUE(version["minor"].isNumeric());
        EXPECT_EQ(version.size(), 2U);
        EXPECT_DOUBLE_EQ(version["major"].asDouble(), 3.0);
        EXPECT_DOUBLE_EQ(version["minor"].asDouble(), 2.0);
    }

    TEST(Response, EmptyCollectionsKeepTheirContractTypes) {
        const auto extractors = api_v1::toJson(api_v1::dto::ExtractorList{});
        for (const auto key : {"note", "tempo", "pitch", "separation"}) {
            EXPECT_TRUE(extractors[key].isArray());
            EXPECT_TRUE(extractors[key].empty());
        }

        const auto separator = api_v1::toJson(api_v1::dto::SeparationExtractorInfo{});
        EXPECT_TRUE(separator["tracks"].isArray());
        EXPECT_TRUE(separator["tracks"].empty());
        EXPECT_FALSE(api_v1::toJson(api_v1::dto::ExtractorInfo{}).isMember("tracks"));
        const auto parameters = api_v1::toJson(api_v1::dto::ParameterResponse{});
        EXPECT_TRUE(parameters["output"]["parameters"].isObject());
        EXPECT_FALSE(parameters.isMember("state"));
        const auto group = api_v1::toJson(api_v1::dto::GroupMetadata{});
        EXPECT_TRUE(group["languages"].isObject());
        EXPECT_TRUE(group["languages"].empty());
        EXPECT_EQ(group["durationMode"].asString(), "skip");
        EXPECT_TRUE(group["parameterPipeline"].isObject());
        EXPECT_TRUE(group["audioDependencies"].isArray());
        EXPECT_TRUE(group.isMember("archSpecificInfo"));
        EXPECT_TRUE(group["archSpecificInfo"].isNull());
        EXPECT_FALSE(group.isMember("defaultArchExtra"));
        const auto architecture = api_v1::toJson(api_v1::dto::ArchitectureMetadata{});
        EXPECT_TRUE(architecture.isMember("defaultArchExtra"));
        EXPECT_TRUE(architecture["defaultArchExtra"].isNull());
    }

    TEST(Response, ValidationProblemsContainReasonsAndPointers) {
        const auto response = http::problemResponse(http::ValidationError({{"/input/notes/0/cent", "maximum", "Too high"}}));
        ASSERT_EQ(response->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_EQ(response->contentTypeString(), "application/problem+json");
        const auto json = response->getJsonObject();
        ASSERT_NE(json, nullptr);
        EXPECT_EQ((*json)["errors"][0]["pointer"].asString(), "/input/notes/0/cent");
        EXPECT_EQ((*json)["errors"][0]["detail"].asString(), "Too high");
    }

    TEST(Response, InternalCauseIsNotPublishedInProblemDetails) {
        http::RequestErrors error{http::HttpBoundaryError("Parser failure", std::make_shared<ExternalError>("private resource location", NativeDiagnostic{"test", "parse", "native", 5}))};
        const auto response = http::problemResponse(error);
        EXPECT_EQ(response->statusCode(), drogon::k500InternalServerError);
        EXPECT_EQ(response->body().find("private resource location"), std::string_view::npos);
    }

    TEST(Response, ArchitectureErrorsUseExplicitStatusesAndKeepNativeCausesPrivate) {
        const auto missing = api_v1::architectureProblemResponse(arch::CatalogErrors{arch::ResourceNotFoundError("Missing group")});
        EXPECT_EQ(missing->statusCode(), drogon::k404NotFound);
        const auto invalid = api_v1::architectureProblemResponse(arch::EnvironmentErrors{arch::InvalidInputError("Invalid speaker")});
        EXPECT_EQ(invalid->statusCode(), drogon::k422UnprocessableEntity);
        EXPECT_EQ((*invalid->getJsonObject())["detail"].asString(), "Invalid speaker");
        const auto failure = api_v1::architectureProblemResponse(arch::SynthesisErrors{arch::OperationError("Failed model execution", std::make_shared<ExternalError>("private model path", NativeDiagnostic{"dsinfer", "execute", "native", 7}))});
        EXPECT_EQ(failure->statusCode(), drogon::k500InternalServerError);
        EXPECT_EQ(failure->body().find("private model path"), std::string_view::npos);
        EXPECT_EQ((*failure->getJsonObject())["detail"].asString(), "Architecture operation failed");
    }

    TEST(Response, AstAndParameterPresentationUseSchemaFieldNames) {
        api_v1::dto::ParameterPresentation parameter;
        parameter.displayValueMappingExpression.value = api_v1::dto::AST::Operation{"*", {api_v1::dto::AST{12800.0}, api_v1::dto::AST{api_v1::dto::AST::Operation{"x", {}}}}};
        const auto json = api_v1::toJson(parameter);
        EXPECT_EQ(json["displayValueMappingExpression"]["operator"].asString(), "*");
        EXPECT_TRUE(json.isMember("fallbackOnDefaultValue"));
        EXPECT_TRUE(json.isMember("displayValueDecimalPlaces"));
    }

}
