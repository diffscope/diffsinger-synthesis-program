#ifndef DSSP_HTTP_RESPONSE_H
#define DSSP_HTTP_RESPONSE_H

#include <functional>
#include <string>
#include <string_view>

#include <drogon/HttpResponse.h>

#include <dssphttp/RequestLifetime.h>
#include <dssphttp/HttpErrors.h>

namespace dssp::http {
    using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
    drogon::HttpResponsePtr taskNotAcceptableResponse(const ResponseContext &context);
    std::string serializationProblemJson(const ResponseContext &context);
    drogon::HttpResponsePtr jsonResponse(const Json::Value &body, const ResponseContext &context = {});
    drogon::HttpResponsePtr problemResponse(int status, std::string_view type, std::string_view title, std::string detail, const ResponseContext &context = {});
    drogon::HttpResponsePtr problemResponse(const RequestErrors &error, const ResponseContext &context = {});
    drogon::HttpResponsePtr problemResponse(const ValidationError &error, const ResponseContext &context = {});
}

#endif // DSSP_HTTP_RESPONSE_H
