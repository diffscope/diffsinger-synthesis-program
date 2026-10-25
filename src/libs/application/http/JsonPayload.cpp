#include "JsonPayload.h"

#include <json/reader.h>
#include <stdcorelib/str.h>

#include <dssphttp/MediaType.h>

namespace dssp::http {

    std::expected<Json::Value, PayloadErrors> readJsonPayload(const drogon::HttpRequest &request) {
        const auto &encoding = request.getHeader("content-encoding");
        if (!encoding.empty() && !stdc::str::equals_insensitive(stdc::str::trim(encoding, " \t"), "identity")) {
            return fail<PayloadErrors>(UnsupportedPayloadError("Unsupported HTTP Content-Encoding"));
        }

        const auto contentType = matchesJsonContentType(request.getHeader("content-type"));
        if (!contentType) {
            return fail<PayloadErrors>(contentType.error());
        }
        if (!*contentType) {
            return fail<PayloadErrors>(UnsupportedPayloadError("Content-Type must be application/json"));
        }

        const auto body = request.body();
        if (body.empty()) {
            return fail<PayloadErrors>(MalformedJsonError("Request body must contain JSON"));
        }

        // JSONcpp is Drogon's JSON library. Strict mode rejects comments, duplicate
        // keys, trailing data and non-JSON numbers. Scalars are valid JSON and reach
        // DTO validation, which requires an object.
        try {
            Json::CharReaderBuilder builder;
            Json::CharReaderBuilder::strictMode(&builder.settings_);
            builder["strictRoot"] = false;
            builder["allowSpecialFloats"] = false;
            builder["stackLimit"] = 128;
            const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value value;
            std::string reason;
            try {
                if (!reader->parse(body.data(), body.data() + body.size(), &value, &reason)) {
                    return fail<PayloadErrors>(MalformedJsonError(std::move(reason)));
                }
            } catch (const Json::RuntimeError &error) {
                // JSONcpp throws for excessive nesting rather than returning false.
                return fail<PayloadErrors>(MalformedJsonError(error.what(), std::make_shared<ExternalError>(error.what(), NativeDiagnostic{"jsoncpp", "parse request", "exception"})));
            }

            return value;
        } catch (const Json::Exception &error) {
            return fail<PayloadErrors>(HttpBoundaryError("JSON parser failed", std::make_shared<ExternalError>(error.what(), NativeDiagnostic{"jsoncpp", "parse request", "exception"})));
        }
    }

}
