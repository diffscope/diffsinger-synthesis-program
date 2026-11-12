#ifndef DSSP_API_V1_CONTROLLERS_ARCHITECTUREDISPATCH_H
#define DSSP_API_V1_CONTROLLERS_ARCHITECTUREDISPATCH_H

#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <stdcorelib/support/logging.h>

#include <dsspapiv1/AsyncReply.h>
#include <dsspapiv1/ArchitectureErrors.h>
#include <dsspapiv1/ArchitectureMapping.h>
#include <dsspapiv1/RequestCodec.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dssparch/ArchitectureCatalog.h>
#include <dssphttp/RequestLifetime.h>
#include <dssphttp/Response.h>
#include <dssphttp/ResponseNegotiation.h>
#include <dssphttp/TaskStream.h>
#include <dsspresource/Async.h>

namespace dssp::api_v1::detail {

    inline stdc::LogCategory dispatchLog("dssp.api_v1.ArchitectureDispatch");

    inline std::string unknownArchitecture(std::string_view id, const localization::Context &locale) {
        return locale.text("api_v1.unknown_architecture", "Unsupported architecture: {0}", id);
    }

    inline drogon::HttpResponsePtr unknownProcessArchitecture(std::string_view id, const http::ResponseContext &context = {}) {
        return architectureProblemResponse(arch::SynthesisErrors{arch::InvalidInputError(unknownArchitecture(id, context.locale))}, context);
    }

    inline arch::CatalogOptions catalogOptions(const http::ResponseContext &context) {
        return {std::string(context.locale.acceptLanguage()), context.locale};
    }

    struct DtoPresenter {
        template <class T>
        auto operator()(T value) const { return toDto(std::move(value)); }
    };

    template <class T, class Errors, class Present = DtoPresenter>
    void respond(std::expected<T, Errors> result, const http::Callback &callback, Present present = {}, const http::ResponseContext &context = {}) {
        if (!result) {
            callback(architectureProblemResponse(result.error(), context));
            return;
        }

        callback(http::jsonResponse(toJson(present(std::move(*result))), context));
    }

    template <class Parameters, class Method, class Present = DtoPresenter>
    void catalog(const arch::ArchitectureCatalog &architectures, const http::Callback &callback, std::string_view id, Parameters parameters, Method method, Present present = {}, const http::ResponseContext &context = {}) {
        const auto selected = architectures.find(id);
        if (!selected) {
            callback(architectureProblemResponse(arch::CatalogErrors{arch::ResourceNotFoundError(unknownArchitecture(id, context.locale))}, context));
            return;
        }

        respond(std::invoke(method, *selected, parameters), callback, std::move(present), context);
    }

    template <class Method, class Present = DtoPresenter>
    void catalogMedia(const arch::ArchitectureCatalog &architectures, const drogon::HttpRequestPtr &request, const http::Callback &callback, std::string_view id, std::string singer, Method method, const http::ResponseContext &context, Present present = {}) {
        auto query = mediaQuery(*request, context.locale);
        if (!query) {
            callback(http::problemResponse(widen_errors<http::RequestErrors>(std::move(query.error())), context));
            return;
        }

        catalog(architectures, callback, id, arch::SingerMediaQuery{{std::move(singer), catalogOptions(context)}, toArchitecture(std::move(*query))}, method, std::move(present), context);
    }

    template <class Response, class Output>
    arch::Completion<Output> completion(http::Callback callback, http::RequestContext context, arch::ArchitecturePtr implementation = {}, std::string_view operation = "synthesis") {
        const auto response = context.responseContext();
        const auto reply = std::make_shared<AsyncReply>(std::move(callback), std::move(context), std::move(implementation), operation);
        return [reply, response](arch::SynthesisResult<Output> result) {
            if (!reply->claim()) {
                return;
            }

            try {
                if (!result) {
                    reply->send(architectureProblemResponse(result.error(), response));
                    return;
                }

                reply->send(http::jsonResponse(toJson(Response{toDto(std::move(*result))}), response));
            } catch (const std::exception &error) {
                const http::RequestErrors failure{http::HttpBoundaryError("Cannot encode an asynchronous HTTP response", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"Drogon/JsonCpp", "encode asynchronous response", "exception"}))};
                reply->send(http::problemResponse(failure, response));
            }
        };
    }

    template <class Request, class Response, class Output, class Start>
    drogon::Task<> process(drogon::HttpRequestPtr request, http::Callback callback, std::shared_ptr<http::RequestLifetimeRegistry> lifetime, std::shared_ptr<const arch::ArchitectureCatalog> architectures, Start start, std::string_view operation = "synthesis", std::chrono::seconds heartbeatInterval = std::chrono::seconds(10)) {
        const auto context = lifetime->track(*request);
        if (context.stopRequested()) {
            co_return;
        }

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
        if (context.stopRequested()) {
            co_return;
        }
        if (!input) {
            callback(http::problemResponse(input.error(), context.responseContext()));
            co_return;
        }

        const auto selected = architectures->find(input->context.arch);
        if (!selected) {
            callback(unknownProcessArchitecture(input->context.arch, context.responseContext()));
            co_return;
        }

        auto parameters = toArchitecture(std::move(*input));
        parameters.context.locale = context.responseContext().locale;
        if (stream) parameters.progress = stream->progress();
        if (context.stopRequested()) {
            co_return;
        }

        dispatchLog.stdcTrace("%1 operation=%2 dispatching to architecture=%3", context.logContext().description(), operation, selected->id());
        const auto reply = std::make_shared<AsyncReply>(std::move(callback), context, selected, operation);
        auto result = co_await resource::awaitCallback<arch::SynthesisResult<Output>>([selected, parameters = std::move(parameters), context, start](auto complete) mutable {
            try {
                std::invoke(start, *selected, std::move(parameters), context.cancellation(), complete);
            } catch (const std::exception &error) {
                complete(arch::SynthesisResult<Output>{fail<arch::SynthesisErrors>(arch::OperationError("Cannot dispatch synthesis work", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"architecture", "dispatch synthesis", "exception"})))});
            }
        });
        if (!reply->claim()) co_return;
        try {
            if (!result) reply->send(architectureProblemResponse(result.error(), context.responseContext()));
            else reply->send(http::jsonResponse(toJson(Response{toDto(std::move(*result))}), context.responseContext()));
        } catch (const std::exception &error) {
            const http::RequestErrors failure{http::HttpBoundaryError("Cannot encode an asynchronous HTTP response", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"Drogon/JsonCpp", "encode asynchronous response", "exception"}))};
            reply->send(http::problemResponse(failure, context.responseContext()));
        }
    }

}

#endif // DSSP_API_V1_CONTROLLERS_ARCHITECTUREDISPATCH_H
