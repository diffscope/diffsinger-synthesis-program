#include "ArchitectureErrors.h"

#include <stdcorelib/support/logging.h>

#include <dssphttp/Response.h>

namespace dssp::api_v1 {
    namespace {
        stdc::LogCategory log("dssp.api_v1.ArchitectureErrors");

        template <class Errors>
        drogon::HttpResponsePtr present(const Errors &errors, const http::ResponseContext &context) {
            return errors.visit(Overload{
                [&context](const arch::ResourceNotFoundError &error) {
                    return http::problemResponse(404, "resource-not-found", context.locale.text("http.title_resource_not_found", "Resource not found"), error.message(), context);
                },
                [&context](const arch::InvalidInputError &error) {
                    auto response = http::problemResponse(422, "invalid-input", context.locale.text("http.title_invalid_input", "Invalid input"), error.message(), context);
                    if (response->statusCode() == drogon::k422UnprocessableEntity) {
                        context.log.problem("invalid-input", error.message());
                    }
                    return response;
                },
                [&context](const arch::OperationError &error) {
                    log.stdcCritical("%1 Architecture operation failed: %2", context.log.description(), http::quoteLogValue(error.formatChain(), 8192));
                    return http::problemResponse(500, "architecture", context.locale.text("http.title_internal_server_error", "Internal server error"), context.locale.text("api_v1.architecture_operation_failed", "Architecture operation failed"), context);
                },
                [&context](const arch::CancelledError &error) {
                    // A stopped request suppresses sending. Otherwise complete
                    // the HTTP request even if the backend reported cancellation.
                    log.stdcCritical("%1 Backend cancelled an operation while the HTTP request was still active: %2", context.log.description(), http::quoteLogValue(error.formatChain(), 8192));
                    return http::problemResponse(500, "cancelled", context.locale.text("http.title_operation_cancelled", "Operation canceled"), context.locale.text("api_v1.architecture_operation_cancelled", "Architecture operation was canceled"), context);
                }});
        }
    }

    drogon::HttpResponsePtr architectureProblemResponse(const arch::CatalogErrors &errors, const http::ResponseContext &context) {
        return present(errors, context);
    }

    drogon::HttpResponsePtr architectureProblemResponse(const arch::EnvironmentErrors &errors, const http::ResponseContext &context) {
        return present(errors, context);
    }

    drogon::HttpResponsePtr architectureProblemResponse(const arch::SynthesisErrors &errors, const http::ResponseContext &context) {
        return present(errors, context);
    }
}
