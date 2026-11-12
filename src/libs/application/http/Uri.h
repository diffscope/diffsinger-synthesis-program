#ifndef DSSP_HTTP_URI_H
#define DSSP_HTTP_URI_H

#include <expected>
#include <string_view>

#include <dssphttp/HttpErrors.h>
#include <dssplocalization/Context.h>

namespace dssp::http {
    class UriSyntaxError final : public ErrorType<UriSyntaxError> {
    public:
        using ErrorType::ErrorType;
        std::string_view kind() const noexcept override { return "dssp.http.uri-syntax"; }
    };

    using UriErrors = ErrorSet<UriSyntaxError, HttpBoundaryError>;

    // RFC 3986 syntax and presence of a scheme only. Scheme negotiation and
    // media parsing remain service responsibilities.
    std::expected<void, UriErrors> validateUri(std::string_view, const localization::Context &locale = {});
}

#endif // DSSP_HTTP_URI_H
