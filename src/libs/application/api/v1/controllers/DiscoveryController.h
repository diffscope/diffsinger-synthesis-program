#ifndef DSSP_API_V1_DISCOVERYCONTROLLER_H
#define DSSP_API_V1_DISCOVERYCONTROLLER_H

#include <memory>
#include <utility>

#include <drogon/HttpController.h>

#include <dssphttp/Response.h>

namespace dssp::api_v1 {
    class DiscoveryController final : public drogon::HttpController<DiscoveryController, false> {
    public:
        explicit DiscoveryController(std::shared_ptr<http::RequestLifetimeRegistry> lifetime = std::make_shared<http::RequestLifetimeRegistry>()) : m_lifetime(std::move(lifetime)) {}

        METHOD_LIST_BEGIN
            ADD_METHOD_TO(DiscoveryController::catalog, "/.well-known/api-catalog", drogon::Get);
            ADD_METHOD_TO(DiscoveryController::specification, "/v1/openapi.json", drogon::Get);
        METHOD_LIST_END

        void catalog(const drogon::HttpRequestPtr &, http::Callback &&) const;
        void specification(const drogon::HttpRequestPtr &, http::Callback &&) const;

    private:
        std::shared_ptr<http::RequestLifetimeRegistry> m_lifetime;
    };
}

#endif // DSSP_API_V1_DISCOVERYCONTROLLER_H
