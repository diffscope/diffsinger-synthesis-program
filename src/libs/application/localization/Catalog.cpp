#include "Catalog.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <utility>

#include <stdcorelib/path.h>
#include <stdcorelib/str.h>
#include <stdcorelib/support/logging.h>
#include <unicode/unistr.h>
#include <unicode/ures.h>

#include <dssplocalization/LocaleMatcher.h>

namespace dssp::localization {
    namespace {
        stdc::LogCategory log("dssp.localization.Catalog");
        using Bundle = std::unique_ptr<UResourceBundle, decltype(&ures_close)>;
        std::expected<std::map<std::string, std::string>, LocalizationError> readBundle(const std::filesystem::path &directory, const std::string &locale) {
            UErrorCode status = U_ZERO_ERROR;
            const auto path = stdc::path::to_utf8(directory);
            Bundle bundle(ures_openDirect(path.c_str(), locale.c_str(), &status), ures_close);
            if (U_FAILURE(status)) {
                return std::unexpected{LocalizationError("Cannot load translation bundle " + locale, std::make_shared<const ExternalError>(u_errorName(status), NativeDiagnostic{"ICU", "ures_openDirect", "UErrorCode", status}))};
            }
            if (ures_getType(bundle.get()) != URES_TABLE) {
                return std::unexpected{LocalizationError("Translation bundle must be a keyed resource table")};
            }

            std::map<std::string, std::string> result;
            for (int32_t i = 0; i < ures_getSize(bundle.get()); ++i) {
                Bundle item(ures_getByIndex(bundle.get(), i, nullptr, &status), ures_close);
                if (U_FAILURE(status) || !item) {
                    return std::unexpected{LocalizationError("Cannot read translation resource", std::make_shared<const ExternalError>(u_errorName(status), NativeDiagnostic{"ICU", "ures_getByIndex", "UErrorCode", status}))};
                }
                int32_t length = 0;
                const auto *value = ures_getString(item.get(), &length, &status);
                if (U_FAILURE(status)) {
                    return std::unexpected{LocalizationError("Translation bundle must contain string resources", std::make_shared<const ExternalError>(u_errorName(status), NativeDiagnostic{"ICU", "ures_getString", "UErrorCode", status}))};
                }
                std::string text;
                icu::UnicodeString(value, length).toUTF8String(text);
                const auto *key = ures_getKey(item.get());
                if (!key) {
                    return std::unexpected{LocalizationError("Translation resource has no key")};
                }
                result.emplace(key, std::move(text));
            }
            return result;
        }
    }

    std::expected<std::shared_ptr<const Catalog>, LocalizationError> Catalog::load(const std::filesystem::path &directory, std::string_view fallbackLocale) {
        try {
            if (log.isLevelEnabled(stdc::Logger::Debug)) {
                log.stdcDebug("Loading translation directory %1 with preferred fallback %2", stdc::path::to_utf8(directory), fallbackLocale);
            }

            auto result = std::shared_ptr<Catalog>(new Catalog);
            result->m_fallbackLocale = fallbackLocale;
            std::error_code error;
            std::filesystem::directory_iterator iterator(directory, error), end;
            // A deployment without translation files may omit the directory.
            if (error == std::errc::no_such_file_or_directory) {
                error.clear();
            }

            std::vector<std::string> locales;
            while (!error && iterator != end) {
                const auto path = iterator->path();
                if (path.extension() == ".res" && path.stem() != "root" && iterator->is_regular_file(error)) {
                    locales.push_back(stdc::path::to_utf8(path.stem()));
                }
                if (!error) {
                    iterator.increment(error);
                }
            }
            if (error) {
                return std::unexpected{LocalizationError("Cannot enumerate translation bundles", std::make_shared<const ExternalError>(error.message(), NativeDiagnostic{"filesystem", "directory_iterator", "error_code", error.value()}))};
            }

            std::ranges::sort(locales);
            for (const auto &locale : locales) {
                auto strings = readBundle(directory, locale);
                if (!strings) {
                    return std::unexpected{std::move(strings.error())};
                }
                result->m_locales.push_back(locale);
                result->m_translations.push_back(std::move(*strings));
            }

            if (locales.empty()) {
                log.stdcDebug("No translation bundles available; display values will use resource keys");
            } else if (std::ranges::find(locales, result->m_fallbackLocale) == locales.end()) {
                log.stdcDebug("Preferred fallback bundle %1 is absent; using available translations", fallbackLocale);
            }

            log.stdcDebug("Translation locales available: %1; directory=%2", stdc::str::join(locales, ", "), stdc::path::to_utf8(directory));
            return std::shared_ptr<const Catalog>(std::move(result));
        } catch (const std::exception &error) {
            return std::unexpected{LocalizationError("Translation resource library failed", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"ICU/filesystem", "load translation resources", "exception"}))};
        }
    }

    std::expected<std::shared_ptr<const Catalog>, LocalizationError> Catalog::load(std::span<const Source> sources, std::string_view fallbackLocale) {
        try {
            std::map<std::string, Strings> translations;
            for (const auto &source : sources) {
                const auto bundle = load(source.directory, fallbackLocale);
                if (!bundle) {
                    return std::unexpected{bundle.error()};
                }

                const auto &catalog = **bundle;
                const auto prefix = std::string(source.module) + '.';
                for (std::size_t i = 0; i < catalog.m_locales.size(); ++i) {
                    const auto &locale = catalog.m_locales[i];
                    auto &strings = translations[locale];
                    for (const auto &[key, value] : catalog.m_translations[i]) {
                        if (!strings.emplace(prefix + key, value).second) {
                            return std::unexpected{LocalizationError("Duplicate translation key " + prefix + key + " for locale " + locale)};
                        }
                    }
                }
            }

            auto result = std::shared_ptr<Catalog>(new Catalog);
            result->m_fallbackLocale = fallbackLocale;
            for (auto &[locale, strings] : translations) {
                result->m_locales.push_back(locale);
                result->m_translations.push_back(std::move(strings));
            }
            return std::shared_ptr<const Catalog>(std::move(result));
        } catch (const std::exception &error) {
            return std::unexpected{LocalizationError("Cannot assemble module translations", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"localization", "assemble translation catalog", "exception"}))};
        }
    }

    std::expected<std::string, LocalizationError> Catalog::text(std::string_view key, std::string_view acceptLanguage) const {
        const std::string resourceKey(key);
        // Only bundles containing this key participate in language matching.
        std::vector<std::string> locales;
        std::vector<const std::string *> strings;
        for (std::size_t i = 0; i < m_locales.size(); ++i) {
            if (const auto it = m_translations[i].find(resourceKey); it != m_translations[i].end()) {
                locales.push_back(m_locales[i]);
                strings.push_back(&it->second);
            }
        }

        if (strings.empty()) {
            // The untranslated display value is deliberately not a contract.
            return resourceKey;
        }

        auto matched = matchLocale(locales, acceptLanguage);
        if (!matched) {
            return std::unexpected{std::move(matched.error())};
        }
        if (*matched) {
            return *strings[**matched];
        }
        if (const auto fallback = std::ranges::find(locales, m_fallbackLocale); fallback != locales.end()) {
            return *strings[static_cast<std::size_t>(fallback - locales.begin())];
        }

        // The preferred fallback bundle is optional, too.
        return *strings.front();
    }

    std::optional<std::size_t> Catalog::select(std::string_view acceptLanguage) const {
        auto matched = matchLocale(m_locales, acceptLanguage);
        if (!matched) {
            log.stdcWarning("Cannot select an error message locale: %1", matched.error().formatChain());
            return std::nullopt;
        }
        return *matched;
    }

    const std::string *Catalog::pattern(const std::string &key, std::optional<std::size_t> locale) const {
        if (locale) {
            const auto &strings = m_translations[*locale];
            if (const auto it = strings.find(key); it != strings.end()) {
                return &it->second;
            }
        }
        const auto fallback = std::ranges::find(m_locales, m_fallbackLocale);
        if (fallback != m_locales.end()) {
            const auto &strings = m_translations[static_cast<std::size_t>(fallback - m_locales.begin())];
            if (const auto it = strings.find(key); it != strings.end()) {
                return &it->second;
            }
        }
        return nullptr;
    }
}
