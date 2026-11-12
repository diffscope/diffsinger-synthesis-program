#include "Uri.h"

#include <string>

#include <stdcorelib/scope_guard.h>
#include <uriparser/Uri.h>

namespace dssp::http {
    std::expected<void, UriErrors> validateUri(std::string_view text, const localization::Context &locale) {
        if (text.empty()) {
            return fail<UriErrors>(UriSyntaxError(locale.text("http.nonempty_uri_expected", "Expected a nonempty absolute URI")));
        }

        UriUriA uri{};
        const char *position = nullptr;
        const auto code = uriParseSingleUriExA(&uri, text.data(), text.data() + text.size(), &position);
        if (code == URI_ERROR_SYNTAX) {
            const auto offset = position ? static_cast<std::size_t>(position - text.data()) : 0;
            return fail<UriErrors>(UriSyntaxError(locale.text("http.invalid_uri_syntax", "Invalid URI syntax at byte {0}", std::to_string(offset))));
        }
        if (code != URI_SUCCESS) {
            return fail<UriErrors>(HttpBoundaryError("URI validation library failed", std::make_shared<const ExternalError>("Cannot parse URI", NativeDiagnostic{"uriparser", "uriParseSingleUriExA", "UriErrorCode", code})));
        }

        const auto cleanup = stdc::make_scope_guard([&] { uriFreeUriMembersA(&uri); });
        if (!uri.scheme.first || uri.scheme.first == uri.scheme.afterLast) {
            return fail<UriErrors>(UriSyntaxError(locale.text("http.uri_scheme_expected", "Expected an absolute URI with a scheme")));
        }

        return {};
    }
}
