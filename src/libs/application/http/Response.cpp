#include "Response.h"

#include <json/writer.h>
#include <stdcorelib/support/logging.h>

namespace dssp::http {
    namespace {
        stdc::LogCategory log("dssp.http.Response");

        drogon::HttpResponsePtr problem(int status, std::string_view type, std::string_view title, std::string detail, const ResponseContext &context, const std::vector<FieldIssue> *issues = nullptr) {
            Json::Value body;
            body["type"] = std::string("urn:dssp:error:") + std::string(type);
            body["title"] = std::string(title);
            body["status"] = status;
            body["detail"] = std::move(detail);

            if (issues) {
                body["errors"] = Json::Value(Json::arrayValue);
                for (const auto &issue : *issues) {
                    Json::Value value;
                    value["pointer"] = issue.pointer;
                    value["errorType"] = issue.errorType;
                    value["detail"] = issue.detail;
                    body["errors"].append(std::move(value));
                }
            }

            auto response = jsonResponse(body, context);
            if (response->statusCode() != drogon::k200OK) {
                return response;
            }

            response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
            response->setContentTypeString("application/problem+json");
            response->addHeader("vary", "Accept-Language");
            context.log.problem(type, title);
            return response;
        }
    }

    drogon::HttpResponsePtr taskNotAcceptableResponse(const ResponseContext &context) {
        const auto title = context.locale.text("http.title_not_acceptable", "Not acceptable");
        auto detail = context.locale.text("http.accept_task_response", "Accept must allow application/json or application/x-ndjson");
        auto response = problem(406, "not-acceptable", title, std::move(detail), context);
        response->addHeader("vary", "Accept, Accept-Language");
        return response;
    }

    std::string serializationProblemJson(const ResponseContext &context) {
        Json::Value body;
        body["type"] = "urn:dssp:error:serialization";
        body["title"] = context.locale.text("http.title_internal_server_error", "Internal server error");
        body["status"] = 500;
        body["detail"] = context.locale.text("http.response_serialization_failed", "Response serialization failed");
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        return Json::writeString(writer, body);
    }

    drogon::HttpResponsePtr jsonResponse(const Json::Value &body, const ResponseContext &context) {
        try {
            auto response = drogon::HttpResponse::newHttpJsonResponse(body);
            // Drogon serializes JSON lazily. Materialize it here to keep JSONcpp
            // exceptions inside the adapter rather than the send callback.
            (void) response->body();
            return response;
        } catch (const Json::Exception &error) {
            // JSONcpp is the only throwing operation in this adapter.
            context.log.problem("serialization", "Response serialization failed");
            log.stdcCritical("%1 JSON response serialization failed: %2", context.log.description(), quoteLogValue(error.what(), 8192));
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k500InternalServerError);
            response->setContentTypeString("application/problem+json");
            response->addHeader("vary", "Accept-Language");
            response->setBody(serializationProblemJson(context));
            return response;
        }
    }

    drogon::HttpResponsePtr problemResponse(const ValidationError &error, const ResponseContext &context) {
        auto response = problem(400, "validation", context.locale.text("http.title_invalid_request", "Invalid request"), context.locale.text("http.request_schema_mismatch", "Request fields do not conform to the API schema"), context, &error.issues());
        if (response->statusCode() == drogon::k400BadRequest) {
            // Field locations and rule names explain a rejection without copying
            // submitted values or parser excerpts into the access record.
            std::string reason = "Invalid request fields:";
            std::size_t count = 0;
            for (const auto &issue : error.issues()) {
                if (++count > 8) {
                    reason += " ...";
                    break;
                }
                reason += " " + issue.pointer.substr(0, 128) + " [" + issue.errorType.substr(0, 64) + "]";
            }
            context.log.problem("validation", reason);
        }

        return response;
    }

    drogon::HttpResponsePtr problemResponse(const RequestErrors &errors, const ResponseContext &context) {
        const auto visitor = Overload{
            [&context](const MalformedJsonError &e) {
                return problem(400, "malformed-json", context.locale.text("http.title_malformed_json", "Malformed JSON"), e.message(), context);
            },
            [&context](const UnsupportedPayloadError &e) {
                auto response = problem(415, "unsupported-payload", context.locale.text("http.title_unsupported_payload", "Unsupported payload"), e.message(), context);
                if (response->statusCode() == drogon::k415UnsupportedMediaType) {
                    context.log.problem("unsupported-payload", e.message());
                }
                return response;
            },
            [&context](const ValidationError &e) { return problemResponse(e, context); },
            [&context](const HttpBoundaryError &e) {
                log.stdcCritical("%1 HTTP adapter failed: %2", context.log.description(), quoteLogValue(e.formatChain(), 8192));
                return problem(500, "http-boundary", context.locale.text("http.title_internal_server_error", "Internal server error"), context.locale.text("http.http_payload_processing_failed", "HTTP payload processing failed"), context);
            }};

        return errors.visit(visitor);
    }

    drogon::HttpResponsePtr problemResponse(int status, std::string_view type, std::string_view title, std::string detail, const ResponseContext &context) {
        return problem(status, type, title, std::move(detail), context);
    }
}
