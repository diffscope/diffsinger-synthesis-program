#ifndef DSSP_APIV1_ARCHITECTUREERRORS_H
#define DSSP_APIV1_ARCHITECTUREERRORS_H

#include <drogon/HttpResponse.h>

#include <dssparch/Errors.h>
#include <dssphttp/Response.h>

namespace dssp::api_v1 {
    drogon::HttpResponsePtr architectureProblemResponse(const arch::CatalogErrors &errors, const http::ResponseContext &context = {});
    drogon::HttpResponsePtr architectureProblemResponse(const arch::EnvironmentErrors &errors, const http::ResponseContext &context = {});
    drogon::HttpResponsePtr architectureProblemResponse(const arch::SynthesisErrors &errors, const http::ResponseContext &context = {});
}

#endif // DSSP_APIV1_ARCHITECTUREERRORS_H
