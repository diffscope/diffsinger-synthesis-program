#ifndef DSSP_LOCALIZATION_CATALOG_H
#define DSSP_LOCALIZATION_CATALOG_H

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dssplocalization/LocalizationError.h>

namespace dssp::localization {
    // ResourceBundle strings are copied at startup into an immutable snapshot.
    // Requests do not mutate a global locale or share mutable ICU bundle cursors.
    class Catalog final {
    public:
        struct Source {
            std::string_view module;
            std::filesystem::path directory;
        };

        // fallbackLocale is an ICU resource basename, such as en_US. No bundle
        // is mandatory; absent translations leave a displayable resource key.
        static std::expected<std::shared_ptr<const Catalog>, LocalizationError> load(const std::filesystem::path &, std::string_view fallbackLocale);
        // Loads module resources into one snapshot with module-qualified keys.
        static std::expected<std::shared_ptr<const Catalog>, LocalizationError> load(std::span<const Source>, std::string_view fallbackLocale);
        std::expected<std::string, LocalizationError> text(std::string_view key, std::string_view acceptLanguage) const;

    private:
        friend class Context;
        std::optional<std::size_t> select(std::string_view acceptLanguage) const;
        const std::string *pattern(const std::string &key, std::optional<std::size_t> locale) const;

        using Strings = std::map<std::string, std::string>;
        std::string m_fallbackLocale;
        std::vector<std::string> m_locales;
        std::vector<Strings> m_translations;
    };
}

#endif // DSSP_LOCALIZATION_CATALOG_H
