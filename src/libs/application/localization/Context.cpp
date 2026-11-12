#include "Context.h"

#include <cstdint>
#include <limits>
#include <vector>

#include <stdcorelib/support/logging.h>
#include <unicode/locid.h>
#include <unicode/msgfmt.h>

#include <dssplocalization/Catalog.h>

namespace dssp::localization {
    namespace {
        stdc::LogCategory log("dssp.localization.Context");
    }

    Context::Context(std::shared_ptr<const Catalog> catalog, std::string_view acceptLanguage)
        : m_catalog(std::move(catalog)), m_acceptLanguage(acceptLanguage), m_locale(m_catalog ? m_catalog->select(acceptLanguage) : std::nullopt) {}

    std::optional<std::string> Context::translate(std::string_view key, std::initializer_list<std::string_view> args) const {
        if (!m_catalog) {
            return std::nullopt;
        }
        const auto *pattern = m_catalog->pattern(std::string(key), m_locale);
        if (!pattern) {
            return std::nullopt;
        }
        if (args.size() == 0) {
            return *pattern;
        }

        std::vector<icu::Formattable> values;
        values.reserve(args.size());
        for (const auto arg : args) {
            if (arg.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
                return std::nullopt;
            }
            values.emplace_back(icu::UnicodeString::fromUTF8(icu::StringPiece(arg.data(), static_cast<int32_t>(arg.size()))));
        }

        UErrorCode status = U_ZERO_ERROR;
        icu::MessageFormat formatter(icu::UnicodeString::fromUTF8(*pattern), icu::Locale::getRoot(), status);
        icu::UnicodeString output;
        icu::FieldPosition position(0);
        if (U_SUCCESS(status)) {
            formatter.format(values.data(), static_cast<int32_t>(values.size()), output, position, status);
        }
        if (U_FAILURE(status)) {
            log.stdcWarning("Cannot format translation %1: %2", key, u_errorName(status));
            return std::nullopt;
        }
        std::string result;
        output.toUTF8String(result);
        return result;
    }
}
