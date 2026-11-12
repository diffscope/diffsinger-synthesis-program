#ifndef DSSP_API_V1_CONTROLLERS_EXTRACTIONDISPATCH_H
#define DSSP_API_V1_CONTROLLERS_EXTRACTIONDISPATCH_H

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include <dsspapiv1/AsyncReply.h>
#include <dsspapiv1/ExtractionErrors.h>
#include <dsspapiv1/ExtractionMapping.h>
#include <dsspapiv1/RequestCodec.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dsspextraction/Service.h>
#include <dssphttp/ResponseNegotiation.h>
#include <dssphttp/TaskStream.h>
#include <dsspresource/Async.h>

namespace dssp::api_v1::detail {
    template <class T, class Map>
    void extractionCatalog(extraction::CatalogResult<T> result, const http::Callback &callback, const http::ResponseContext &context, Map map) {
        if (!result) callback(extractionProblemResponse(result.error(), context));
        else callback(http::jsonResponse(toJson(std::invoke(map, std::move(*result))), context));
    }

    template <class T>
    void extractionCatalog(extraction::CatalogResult<T> result, const http::Callback &callback, const http::ResponseContext &context) {
        extractionCatalog(std::move(result), callback, context, [](T value) { return toDto(std::move(value)); });
    }

    template <class Response, class Output, class Request = dto::ExtractionRequest, class Start>
    drogon::Task<> extract(drogon::HttpRequestPtr request, http::Callback callback, std::shared_ptr<http::RequestLifetimeRegistry> lifetime, std::shared_ptr<extraction::Service> service, Start start, std::string_view operation, std::chrono::seconds heartbeatInterval = std::chrono::seconds(10)) {
        const auto context = lifetime->track(*request);
        if (context.stopRequested()) co_return;
        const auto representation = http::negotiateTaskResponse(request->getHeader("accept"));
        if (representation == http::TaskRepresentation::Unacceptable) {
            callback(http::taskNotAcceptableResponse(context.responseContext()));
            co_return;
        }
        std::shared_ptr<http::TaskStream> stream;
        if (representation == http::TaskRepresentation::Ndjson) {
            stream = http::TaskStream::open(*request, callback, context, heartbeatInterval);
            callback = [stream](const auto &response) { stream->finish(response); };
        } else {
            callback = [send = std::move(callback)](const auto &response) {
                response->addHeader("vary", "Accept, Accept-Language");
                send(response);
            };
        }
        auto input = fromHttp<Request>(*request, context.responseContext().locale);
        if (context.stopRequested()) co_return;
        if (!input) {
            callback(http::problemResponse(input.error(), context.responseContext()));
            co_return;
        }

        const auto reply = std::make_shared<AsyncReply>(std::move(callback), context, service, operation);
        auto parameters = toExtraction(std::move(*input));
        parameters.locale = context.responseContext().locale;
        if (stream) parameters.progress = stream->progress();
        auto result = co_await resource::awaitCallback<extraction::ExtractionResult<Output>>([service, parameters = std::move(parameters), context, start](auto complete) mutable {
            try {
                std::invoke(start, *service, std::move(parameters), context.cancellation(), complete);
            } catch (const std::exception &error) {
                complete(extraction::ExtractionResult<Output>{fail<extraction::ExtractionErrors>(extraction::OperationError(context.responseContext().locale.text("api_v1.extraction_dispatch_failed", "Cannot dispatch extraction work"), std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"extraction", "dispatch extraction", "exception"})))});
            }
        });
        if (!reply->claim()) co_return;
        try {
            if (!result) reply->send(extractionProblemResponse(result.error(), context.responseContext()));
            else reply->send(http::jsonResponse(toJson(Response{toDto(std::move(*result))}), context.responseContext()));
        } catch (const std::exception &error) {
            const http::RequestErrors failure{http::HttpBoundaryError("Cannot encode an extraction HTTP response", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"Drogon/JsonCpp", "encode extraction response", "exception"}))};
            reply->send(http::problemResponse(failure, context.responseContext()));
        }
    }
}

#endif // DSSP_API_V1_CONTROLLERS_EXTRACTIONDISPATCH_H
