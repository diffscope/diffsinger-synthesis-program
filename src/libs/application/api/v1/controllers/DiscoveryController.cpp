#include "DiscoveryController.h"

#include <string>
#include <utility>

namespace dssp::api_v1 {
    namespace {
        constexpr unsigned char openApiSpec[] = {
#include "OpenApiSpecData.inc"
        };
    }

    void DiscoveryController::catalog(const drogon::HttpRequestPtr &request, http::Callback &&callback) const {
        // Root-relative references preserve the external origin behind TLS termination.
        Json::Value catalog;
        Json::Value index;
        index["anchor"] = "/.well-known/api-catalog";
        Json::Value api;
        api["href"] = "/v1";
        index["item"].append(std::move(api));
        catalog["linkset"].append(std::move(index));

        Json::Value service;
        service["anchor"] = "/v1";
        Json::Value description;
        description["href"] = "/v1/openapi.json";
        description["type"] = "application/json";
        service["service-desc"].append(std::move(description));
        catalog["linkset"].append(std::move(service));

        auto response = http::jsonResponse(catalog, http::requestLog(*request));
        if (response->statusCode() == drogon::k200OK) {
            response->setContentTypeString("application/linkset+json; profile=\"https://www.rfc-editor.org/info/rfc9727\"");
        }
        response->addHeader("link", "</.well-known/api-catalog>; rel=\"api-catalog\"; type=\"application/linkset+json\"");
        callback(response);
    }

    void DiscoveryController::specification(const drogon::HttpRequestPtr &, http::Callback &&callback) const {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setContentTypeString("application/json");
        response->setBody(std::string(reinterpret_cast<const char *>(openApiSpec), sizeof(openApiSpec)));
        callback(response);
    }
}
