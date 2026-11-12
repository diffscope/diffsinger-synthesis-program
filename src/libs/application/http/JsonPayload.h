#ifndef DSSP_HTTP_JSONPAYLOAD_H
#define DSSP_HTTP_JSONPAYLOAD_H

#include <drogon/HttpRequest.h>

#include <dssphttp/HttpErrors.h>
#include <dssplocalization/Context.h>

namespace dssp::http {
    std::expected<Json::Value, PayloadErrors> readJsonPayload(const drogon::HttpRequest &request, const localization::Context &locale = {});
}

#endif // DSSP_HTTP_JSONPAYLOAD_H
