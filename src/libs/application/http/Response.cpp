#include "Response.h"

#include <stdcorelib/support/logging.h>

namespace dssp::http {
    namespace {
        stdc::LogCategory log("dssp.http.Response");

        drogon::HttpResponsePtr problem(int status, std::string_view type, std::string_view title, std::string detail, const RequestLog &context, const std::vector<FieldIssue> *issues = nullptr) {
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
            context.problem(type, title);
            return response;
        }
    }

    drogon::HttpResponsePtr jsonResponse(const Json::Value &body, const RequestLog &context) {
        try {
            auto response = drogon::HttpResponse::newHttpJsonResponse(body);
            // Drogon serializes JSON lazily. Materialize it here to keep JSONcpp
            // exceptions inside the adapter rather than the send callback.
            (void) response->body();
            return response;
        } catch (const Json::Exception &error) {
            // JSONcpp is the only throwing operation in this adapter.
            context.problem("serialization", "Response serialization failed");
            log.stdcCritical("%1 JSON response serialization failed: %2", context.description(), quoteLogValue(error.what(), 8192));
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k500InternalServerError);
            response->setContentTypeString("application/problem+json");
            response->setBody(R"({"type":"urn:dssp:error:serialization","title":"Internal server error","status":500,"detail":"Response serialization failed"})");
            return response;
        }
    }

    drogon::HttpResponsePtr problemResponse(const ValidationError &error, const RequestLog &context) {
        auto response = problem(400, "validation", "Invalid request", error.message(), context, &error.issues());
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
            context.problem("validation", reason);
        }

        return response;
    }

    drogon::HttpResponsePtr problemResponse(const RequestErrors &errors, const RequestLog &context) {
        const auto visitor = Overload{
            [&context](const MalformedJsonError &e) {
                return problem(400, "malformed-json", "Malformed JSON", e.message(), context);
            },
            [&context](const UnsupportedPayloadError &e) {
                auto response = problem(415, "unsupported-payload", "Unsupported payload", e.message(), context);
                if (response->statusCode() == drogon::k415UnsupportedMediaType) {
                    context.problem("unsupported-payload", e.message());
                }
                return response;
            },
            [&context](const ValidationError &e) { return problemResponse(e, context); },
            [&context](const HttpBoundaryError &e) {
                log.stdcCritical("%1 HTTP adapter failed: %2", context.description(), quoteLogValue(e.formatChain(), 8192));
                return problem(500, "http-boundary", "Internal server error", "HTTP payload processing failed", context);
            }};

        return errors.visit(visitor);
    }

    drogon::HttpResponsePtr problemResponse(int status, std::string_view type, std::string_view title, std::string detail, const RequestLog &context) {
        return problem(status, type, title, std::move(detail), context);
    }
}
