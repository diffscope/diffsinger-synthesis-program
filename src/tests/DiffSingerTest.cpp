#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <dsspapp/ArchComposition.h>
#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/SingerCatalog.h>
#include <dssparchdiffsinger/SynthesisParameters.h>
#include <dsspmedia/Resources.h>
#include <dssptests/TestFiles.h>

namespace dssp::tests {

    class DiffSingerTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto createdEnvironment = inference::Environment::create({});
            ASSERT_TRUE(createdEnvironment);
            m_environment = *createdEnvironment;
            const auto createdMedia = media::Resources::create(std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "media" / "magic.mgc");
            ASSERT_TRUE(createdMedia);
            m_services = {m_environment, *createdMedia, std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization"};
            const auto createdArchitecture = arch_diffsinger::create(m_services);
            ASSERT_TRUE(createdArchitecture);
            m_implementation = *createdArchitecture;
        }

        std::shared_ptr<inference::Environment> m_environment;
        arch::ArchitectureServices m_services;
        arch::ArchitecturePtr m_implementation;
    };

    TEST_F(DiffSingerTest, CompositionContainsTheCompiledSetAndFactoriesCreateIndependentInstances) {
        const auto catalog = composeArchitectures(m_services);
        ASSERT_TRUE(catalog);
        ASSERT_EQ((*catalog)->entries().size(), 1U);
        const auto implementation = (*catalog)->find(arch_diffsinger::Id);
        ASSERT_NE(implementation, nullptr);
        EXPECT_EQ(implementation->id(), "diffsinger");
        EXPECT_EQ((*catalog)->find("DiffSinger"), nullptr);
        const auto another = arch_diffsinger::create(m_services);
        ASSERT_TRUE(another);
        EXPECT_NE(implementation, *another);
        EXPECT_EQ(implementation->id(), (*another)->id());
    }

    TEST_F(DiffSingerTest, EmptyEnvironmentAdvertisesMetadataAndReportsUnknownResources) {
        EXPECT_TRUE(m_environment->packages().empty());
        const auto metadata = m_implementation->metadata({});
        ASSERT_TRUE(metadata);
        EXPECT_EQ(metadata->id, "diffsinger");
        EXPECT_EQ(metadata->name, "DiffSinger");
        EXPECT_EQ(metadata->parameters.size(), 10U);
        ASSERT_TRUE(metadata->defaultArchExtra.isObject());
        EXPECT_EQ(metadata->defaultArchExtra["steps"]["pitch"].asInt(), 20);
        EXPECT_EQ(metadata->defaultArchExtra["steps"]["variance"].asInt(), 20);
        EXPECT_EQ(metadata->defaultArchExtra["steps"]["acoustic"].asInt(), 20);
        EXPECT_EQ(metadata->defaultArchExtra["depth"].asDouble(), 1);
        for (const auto id : {"pitch", "expressiveness", "energy", "breathiness", "voicing", "tension", "mouth_opening", "gender", "velocity", "tone_shift"}) {
            EXPECT_TRUE(metadata->parameters.contains(id));
            const auto *definition = arch_diffsinger::parameterDefinition(id);
            ASSERT_NE(definition, nullptr);
            EXPECT_DOUBLE_EQ(metadata->parameters.at(id).defaultValue, definition->defaultValue);
        }

        const auto singers = m_implementation->singers({});
        ASSERT_TRUE(singers);
        EXPECT_TRUE(singers->empty());
        const auto singer = m_implementation->singer({});
        ASSERT_FALSE(singer);
        EXPECT_NE(singer.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto groups = m_implementation->groups({});
        ASSERT_TRUE(groups);
        EXPECT_TRUE(groups->empty());
        const auto group = m_implementation->group({});
        ASSERT_FALSE(group);
        EXPECT_NE(group.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto groupSingers = m_implementation->groupSingers({});
        ASSERT_FALSE(groupSingers);
        EXPECT_NE(groupSingers.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto avatar = m_implementation->avatar({});
        ASSERT_FALSE(avatar);
        EXPECT_NE(avatar.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto background = m_implementation->background({});
        ASSERT_FALSE(background);
        EXPECT_NE(background.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto demoAudio = m_implementation->demoAudio({});
        ASSERT_FALSE(demoAudio);
        EXPECT_NE(demoAudio.error().get_if<arch::ResourceNotFoundError>(), nullptr);
        const auto tag = m_implementation->envTag({});
        ASSERT_FALSE(tag);
        EXPECT_NE(tag.error().get_if<arch::InvalidInputError>(), nullptr);
    }

    TEST_F(DiffSingerTest, MissingTranslationFilesDoNotPreventArchitectureMetadata) {
        const TestFiles files;
        auto untranslatedServices = m_services;
        untranslatedServices.translationRoot = files.path();
        const auto untranslated = arch_diffsinger::create(untranslatedServices);
        ASSERT_TRUE(untranslated);
        for (const auto locale : {"", "en-US", "zh-CN"}) {
            const auto metadata = (*untranslated)->metadata({locale});
            ASSERT_TRUE(metadata);
            EXPECT_EQ(metadata->id, "diffsinger");
            EXPECT_FALSE(metadata->name.empty());
            ASSERT_EQ(metadata->parameters.size(), 10U);
            for (const auto &[id, parameter] : metadata->parameters) {
                EXPECT_FALSE(parameter.name.empty()) << id;
            }
        }
    }

    TEST_F(DiffSingerTest, AllSynthesisStagesValidateSingerContext) {
        const auto check = [&](auto submit) {
            const auto promise = std::make_shared<std::promise<void>>();
            auto done = promise->get_future();
            submit([promise](auto result) {
                EXPECT_FALSE(result);
                if (!result) EXPECT_NE(result.error().template get_if<arch::InvalidInputError>(), nullptr);
                promise->set_value();
            });
            EXPECT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        };

        check([&](auto callback) { m_implementation->pronunciation({}, {}, std::move(callback)); });
        check([&](auto callback) { m_implementation->phoneme({}, {}, std::move(callback)); });
        check([&](auto callback) { m_implementation->duration({}, {}, std::move(callback)); });
        check([&](auto callback) { m_implementation->parameter({}, {}, std::move(callback)); });
        check([&](auto callback) { m_implementation->audio({}, {}, std::move(callback)); });
    }

    TEST_F(DiffSingerTest, CancelledRequestDoesNotCancelConcurrentRequests) {
        std::stop_source cancelled;
        cancelled.request_stop();
        bool cancelledReply = false;
        m_implementation->pronunciation({}, cancelled.get_token(), [&](auto result) {
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().template get_if<arch::CancelledError>(), nullptr);
            cancelledReply = true;
        });
        EXPECT_TRUE(cancelledReply);
        std::atomic<unsigned> completed = 0;
        std::vector<std::jthread> workers;
        for (unsigned i = 0; i < 8; ++i) {
            workers.emplace_back([&] {
                m_implementation->pronunciation({}, {}, [&](auto result) {
                    EXPECT_FALSE(result);
                    ++completed;
                });
            });
        }

        workers.clear();
        EXPECT_EQ(completed.load(), 8U);
    }

    TEST(DiffSinger, MissingWolfDefaultLyricRemainsEmptyForEveryLanguage) {
        arch_diffsinger::GroupRecord record;
        record.info.id = "group";
        for (const auto handle : {"cmn", "eng", "jpn", "yue", "deu"}) {
            record.info.languages.emplace(handle, arch::GroupLanguageInfo{});
            record.languageNames.emplace(handle, srt::DisplayText(handle));
        }

        const auto info = arch_diffsinger::groupInfo(record, {});
        ASSERT_TRUE(info);
        ASSERT_EQ(info->languages.size(), record.languageNames.size());
        for (const auto &[handle, language] : info->languages) {
            EXPECT_EQ(language.name, handle);
            EXPECT_TRUE(language.defaultLyric.empty());
        }
    }

    TEST(DiffSinger, GroupLocalizationRetainsEachLanguageCapabilityAndTheCommittedSnapshot) {
        arch_diffsinger::GroupRecord record;
        record.info.id = "group";
        record.info.languages.emplace("eng", arch::GroupLanguageInfo{"eng", "supplied lyric", arch::PronunciationMode::Full, arch::PhonemeMode::Full});
        record.info.languages.emplace("cmn", arch::GroupLanguageInfo{"cmn", {}, arch::PronunciationMode::Skip, arch::PhonemeMode::TokenOnly});
        record.languageNames.emplace("eng", srt::DisplayText("English"));
        record.languageNames.emplace("cmn", srt::DisplayText("Mandarin"));
        const auto info = arch_diffsinger::groupInfo(record, {});
        ASSERT_TRUE(info);
        EXPECT_EQ(info->languages.at("eng").name, "English");
        EXPECT_EQ(info->languages.at("eng").defaultLyric, "supplied lyric");
        EXPECT_EQ(info->languages.at("eng").phonemeMode, arch::PhonemeMode::Full);
        EXPECT_EQ(info->languages.at("cmn").name, "Mandarin");
        EXPECT_EQ(info->languages.at("cmn").pronunciationMode, arch::PronunciationMode::Skip);
        EXPECT_EQ(info->languages.at("cmn").phonemeMode, arch::PhonemeMode::TokenOnly);
        EXPECT_EQ(record.info.languages.at("eng").name, "eng");
        EXPECT_EQ(record.info.languages.at("cmn").name, "cmn");
    }

    TEST(DiffSinger, LogicalSpeakersMustHaveValidEmbeddingsInEveryConsumingStage) {
        arch_diffsinger::StageRecord duration, acoustic, pitch;
        duration.usesSpeakers = true;
        duration.speakers = {"dur-first", "dur-second"};
        duration.embeddedSpeakers = {"dur-first", "dur-second"};
        duration.speakerMapping = {{"main", "dur-first"}, {"soft", "dur-second"}};
        acoustic.usesSpeakers = true;
        acoustic.speakers = {"ac-main", "ac-broken"};
        acoustic.embeddedSpeakers = {"ac-main"};
        acoustic.speakerMapping = {{"main", "ac-main"}, {"soft", "ac-broken"}, {"other", "undeclared"}};
        // A stage without embeddings has no speaker condition to intersect.
        const auto common = arch_diffsinger::commonSpeakers({{"duration", duration}, {"acoustic", acoustic}, {"pitch", pitch}});
        EXPECT_EQ(common, std::vector<std::string>{"main"});
        acoustic.speakerMapping.clear();
        duration.speakerMapping = {{"ac-main", "dur-first"}};
        EXPECT_EQ(arch_diffsinger::commonSpeakers({{"duration", duration}, {"acoustic", acoustic}}), std::vector<std::string>{"ac-main"});
        acoustic.embeddedSpeakers.clear();
        EXPECT_TRUE(arch_diffsinger::commonSpeakers({{"duration", duration}, {"acoustic", acoustic}}).empty());
    }

}
