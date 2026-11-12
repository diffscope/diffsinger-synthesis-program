#ifndef DSSP_API_V1_CONTROLLERS_CONTROLLERSUPPORT_H
#define DSSP_API_V1_CONTROLLERS_CONTROLLERSUPPORT_H

#include <dsspapiv1/RequestCodec.h>
#include <dsspapiv1/ResponseCodec.h>
#include <dssphttp/RequestLifetime.h>
#include <dssphttp/Response.h>

namespace dssp::api_v1::detail {

    template <class Response>
    void placeholder(const drogon::HttpRequestPtr &request, const http::Callback &callback, const std::shared_ptr<http::RequestLifetimeRegistry> &lifetime) {
        const auto context = lifetime->track(*request);
        if (context.stopRequested()) {
            return;
        }

        callback(http::jsonResponse(toJson(Response{}), context.responseContext()));
    }

    template <class Request, class Response>
    void processPlaceholder(const drogon::HttpRequestPtr &request, const http::Callback &callback, const std::shared_ptr<http::RequestLifetimeRegistry> &lifetime) {
        const auto context = lifetime->track(*request);
        if (context.stopRequested()) {
            return;
        }

        auto input = fromHttp<Request>(*request, context.responseContext().locale);
        if (context.stopRequested()) {
            return;
        }
        if (!input) {
            callback(http::problemResponse(input.error(), context.responseContext()));
            return;
        }

        // Future service calls receive *input and context. In asynchronous handlers,
        // move both into the completion state and check cancellation before replying.
        callback(http::jsonResponse(toJson(Response{}), context.responseContext()));
    }

}

#endif // DSSP_API_V1_CONTROLLERS_CONTROLLERSUPPORT_H
