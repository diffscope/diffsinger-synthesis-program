#include <atomic>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dsspinference/Display.h>
#include <dssplocalization/Catalog.h>
#include <dssplocalization/Context.h>
#include <dssplocalization/LocaleMatcher.h>
#include <dssptests/TestFiles.h>

namespace dssp::tests {
    TEST(Localization, CldrMatchesRegionScriptAndLanguageAliases) {
        const std::vector<std::string> locales{"en-GB", "zh-Hans", "zh-Hant", "ja"};
        for (const auto &[header, expected] : {std::pair{"en-AU", 0U}, {"cmn-Hans-CN", 1U}, {"zh-TW", 2U}, {"ja-JP", 3U}, {"de;q=1, ja;q=0.9", 3U}, {"en;q=0,ja;q=0.5", 3U}, {"*,ja;q=0.9", 3U}}) {
            SCOPED_TRACE(header);
            const auto matched = localization::matchLocale(locales, header);
            ASSERT_TRUE(matched);
            ASSERT_TRUE(*matched);
            EXPECT_EQ(**matched, expected);
        }

        for (const auto header : {"", "*", "ja;q=0", "de", "ja;q=wrong"}) {
            SCOPED_TRACE(header);
            const auto matched = localization::matchLocale(locales, header);
            ASSERT_TRUE(matched);
            EXPECT_FALSE(*matched);
        }
    }

    TEST(Localization, DisplayUsesDeclarationDefaultAndNormalizesLegacyLocaleNames) {
        const srt::DisplayText display("Default", {{"zh_CN.UTF-8", "中文"}, {"ja_JP", "日本語"}});
        const auto chinese = inference::localize(display, "zh-CN");
        ASSERT_TRUE(chinese);
        EXPECT_EQ(*chinese, "中文");
        const auto fallback = inference::localize(display, "de");
        ASSERT_TRUE(fallback);
        EXPECT_EQ(*fallback, "Default");
        const auto stableId = inference::localize({}, "ja", "stable-id");
        ASSERT_TRUE(stableId);
        EXPECT_EQ(*stableId, "stable-id");
        const auto emptyTranslation = inference::localize(srt::DisplayText("Default", {{"ja", ""}}), "ja");
        ASSERT_TRUE(emptyTranslation);
        EXPECT_EQ(*emptyTranslation, "Default");
    }

    TEST(Localization, BundlesAreExtensibleAndConcurrentRequestsKeepTheirOwnLocale) {
        const auto catalog = localization::Catalog::load(std::filesystem::path(DSSP_TEST_TRANSLATION_DIR), "en_US");
        ASSERT_TRUE(catalog);
        // zh_CN supplies Hans text; unmatched requests use the explicit English fallback.
        const auto expectations = {std::pair{"ja-JP", "日本語"}, {"zh-CN", "中文"}, {"zh", "中文"}, {"zh-TW", "English"}, {"de", "English"}, {"", "English"}, {"en-US", "English"}, {"en-GB", "English"}, {"en-US;q=1,zh-CN;q=0.5", "English"}, {"zh-CN;q=1,en-US;q=0.5", "中文"}};
        for (const auto &[locale, title] : expectations) {
            SCOPED_TRACE(locale);
            const auto value = (*catalog)->text("title", locale);
            ASSERT_TRUE(value) << (value ? "" : value.error().formatChain());
            EXPECT_EQ(*value, title);
        }

        std::atomic<unsigned> failures = 0;
        std::vector<std::jthread> workers;
        for (const auto &[locale, title] : expectations) {
            workers.emplace_back([&, locale, title] {
                for (unsigned i = 0; i < 16; ++i) {
                    const auto value = (*catalog)->text("title", locale);
                    if (!value || *value != title) {
                        ++failures;
                    }
                }
            });
        }

        workers.clear();
        EXPECT_EQ(failures.load(), 0U);
        const auto missing = (*catalog)->text("missing", "ja");
        ASSERT_TRUE(missing);
        EXPECT_FALSE(missing->empty());
        const auto incomplete = (*catalog)->text("fallbackOnly", "ja");
        ASSERT_TRUE(incomplete);
        EXPECT_EQ(*incomplete, "English only");
    }

    TEST(Localization, ModuleCatalogsResolveQualifiedKeysAndUseTheirOwnFallback) {
        const auto directory = std::filesystem::path(DSSP_TEST_TRANSLATION_DIR);
        const TestFiles files;
        ASSERT_TRUE(std::filesystem::copy_file(directory / "en_US.res", files.path() / "en_US.res"));

        const localization::Catalog::Source sources[]{
            {"first", directory},
            {"second", files.path()},
        };
        const auto catalog = localization::Catalog::load(sources, "en_US");
        ASSERT_TRUE(catalog);
        const localization::Context chinese(*catalog, "zh-CN");
        EXPECT_EQ(chinese.text("first.title", "Default"), "中文");
        EXPECT_EQ(chinese.text("second.title", "Default"), "English");
        EXPECT_EQ(chinese.text("third.title", "Default"), "Default");
    }

    TEST(Localization, EmptyOrAbsentTranslationDirectoryStillProducesDisplayText) {
        const TestFiles files;
        for (const auto &directory : {files.path(), files.path() / "absent"}) {
            SCOPED_TRACE(directory.string());
            const auto catalog = localization::Catalog::load(directory, "en_US");
            ASSERT_TRUE(catalog) << (catalog ? "" : catalog.error().formatChain());
            for (const auto locale : {"", "en-US", "zh-CN", "de"}) {
                const auto value = (*catalog)->text("pitch", locale);
                ASSERT_TRUE(value);
                // The untranslated text is unspecified; don't assert its exact value.
                EXPECT_FALSE(value->empty());
            }
        }
    }

    TEST(Localization, MissingPreferredFallbackBundleDoesNotPreventDisplay) {
        const auto catalog = localization::Catalog::load(std::filesystem::path(DSSP_TEST_TRANSLATION_DIR), "fr_FR");
        ASSERT_TRUE(catalog);
        const auto value = (*catalog)->text("title", "de");
        ASSERT_TRUE(value);
        EXPECT_FALSE(value->empty());
        const auto chinese = (*catalog)->text("title", "zh-CN");
        ASSERT_TRUE(chinese);
        EXPECT_EQ(*chinese, "中文");
    }

    TEST(Localization, DamagedExistingBundleStillReturnsAnError) {
        const TestFiles files;
        files.write("en_US.res", "invalid ICU resource data");
        const auto catalog = localization::Catalog::load(files.path(), "en_US");
        ASSERT_FALSE(catalog);
        ASSERT_NE(catalog.error().cause(), nullptr);
        EXPECT_EQ(catalog.error().cause()->kind(), "dssp.external");
    }
}
