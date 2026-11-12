#ifndef DSSP_LOCALIZATION_CONTEXT_H
#define DSSP_LOCALIZATION_CONTEXT_H

#include <cstddef>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dssp::localization {
    class Catalog;

    // Retains the request language and the immutable error message catalog.
    class Context {
    public:
        Context() = default;
        Context(std::shared_ptr<const Catalog>, std::string_view acceptLanguage);

        std::string_view acceptLanguage() const noexcept { return m_acceptLanguage; }

        // Returns the selected translation or formats the English fallback.
        template <class... Args>
        std::string text(std::string_view key, std::format_string<Args...> fallback, Args &&...args) const {
            if (auto translated = translate(key, {std::string_view(args)...})) {
                return std::move(*translated);
            }
            return std::format(fallback, std::forward<Args>(args)...);
        }

    private:
        std::optional<std::string> translate(std::string_view key, std::initializer_list<std::string_view> args) const;

        std::shared_ptr<const Catalog> m_catalog;
        std::string m_acceptLanguage;
        std::optional<std::size_t> m_locale;
    };
}

#endif // DSSP_LOCALIZATION_CONTEXT_H
