#include <algorithm>
#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dsinfer/Inference/InferenceDriverPlugin.h>
#include <gtest/gtest.h>
#include <synthrt/Core/SynthUnit.h>

#include <dssparchdiffsinger/DiffSinger.h>
#include <dssparchdiffsinger/Factory.h>
#include <dsspruntime/InferenceCaches.h>
#include <dsspcodecutil/codecutil.h>
#include <dssptests/TestFiles.h>

namespace dssp::tests {
    namespace {
        constexpr char gif[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";

        void writeSingerPackage(const TestFiles &files, std::string_view directory, std::string_view version, bool predictiveStages = false) {
            const std::filesystem::path root(directory);
            const std::string stageDeclarations = predictiveStages ? R"(,{"id":"duration","path":"duration.json"},{"id":"pitch","path":"pitch.json"},{"id":"variance","path":"variance.json"})" : "";
            files.write(root / "desc.json", "{\"$version\":\"1.0\",\"id\":\"example/singers\",\"version\":\"" + std::string(version) + R"(","runtimeLevel":1,"contributions":{
                "inference":[{"id":"acoustic","path":"acoustic.json"},{"id":"vocoder","path":"vocoder.json"},{"id":"g2p","path":"g2p.json"})" + stageDeclarations + R"(],
                "linguist":[{"id":"eng","path":"eng.json"}],
                "singer":[{"id":"first","path":"first.json"},{"id":"second","path":"second.json"}]}})");
            files.write(root / "acoustic.json", R"({"interface":"org.openvpi.dsinfer.inference.Acoustic","variant":"onnx","level":1,
                "exports":{"varianceControls":["energy"],"transitionControls":["gender","velocity"]},
                "configuration":{"model":"${root}/missing-acoustic.onnx","phonemes":"phonemes.json","parameters":["energy","gender","velocity"]}})");
            files.write(root / "vocoder.json", R"({"interface":"org.openvpi.dsinfer.inference.Vocoder","variant":"onnx","level":1,
                "exports":{},"configuration":{"model":"${root}/missing-vocoder.onnx","pitchControllable":true}})");
            files.write(root / "phonemes.json", R"({"a":0,"SP":1,"AP":2})");
            if (predictiveStages) {
                files.write(root / "duration.json", R"({"interface":"org.openvpi.dsinfer.inference.Duration","variant":"onnx","level":1,
                    "exports":{},"configuration":{"encoder":"${root}/missing-duration-encoder.onnx","predictor":"${root}/missing-duration-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json"}})");
                files.write(root / "pitch.json", R"({"interface":"org.openvpi.dsinfer.inference.Pitch","variant":"onnx","level":1,
                    "exports":{"allowExpressiveness":true},"configuration":{"encoder":"${root}/missing-pitch-encoder.onnx","predictor":"${root}/missing-pitch-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json","useExpressiveness":true}})");
                files.write(root / "variance.json", R"({"interface":"org.openvpi.dsinfer.inference.Variance","variant":"onnx","level":1,
                    "exports":{"predictions":["energy"]},"configuration":{"encoder":"${root}/missing-variance-encoder.onnx","predictor":"${root}/missing-variance-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json"}})");
            }
            files.write(root / "g2p.json", R"({"interface":"org.openvpi.wolf.inference.G2P","variant":"pipe-chain","level":1,
                "exports":{"languages":[{"language":"eng","scheme":"test"}],"symbols":["a"]},
                "configuration":{"formatVersion":1,"steps":[{"step":"fallback","params":{}}]}})");
            files.write(root / "eng.json", R"({"interface":"org.openvpi.wolf.linguist.WolfLinguist","variant":"wolf","level":1,
                "language":"eng","scheme":"test","name":{"_":"English","ja":"英語"},
                "exports":{"phonemes":["a"]},"configuration":{},"imports":[{"role":"linguist/g2p","ref":":inference/g2p"}]})");
            for (const auto id : {"first", "second"}) {
                const std::string languageRole = std::string_view(id) == "first" ? "language/eng" : "custom/english";
                const std::string stageImports = predictiveStages ? R"(
                        {"role":"singer/duration","ref":":inference/duration","options":{}},
                        {"role":"singer/pitch","ref":":inference/pitch","options":{}},
                        {"role":"singer/variance","ref":":inference/variance","options":{"predictions":["energy"]}},)" : "";
                files.write(root / (std::string(id) + ".json"), R"({"interface":"org.openvpi.dsinfer.singer.DiffSinger","variant":"openvpi","level":1,
                    "name":{"_":"Singer","zh":"歌手"},"avatar":{"_":"pixel.bin","zh":"missing-image.bin"},"demoAudio":"", "configuration":{"dict":"unused-dictionary.txt"},
                    "languages":{"eng":")" + languageRole + R"("},"defaultLanguage":"eng","imports":[
                        {"role":"singer/acoustic","ref":":inference/acoustic","options":{}},
                        {"role":"singer/vocoder","ref":":inference/vocoder","options":{}},)" + stageImports + R"(
                        {"role":")" + languageRole + R"(","ref":":linguist/eng","options":{}}]})");
            }
            files.write(root / "pixel.bin", std::string_view(gif, sizeof(gif) - 1));
        }
    }

    class SingerDiscoveryTest : public ::testing::Test {
    protected:
        void SetUp() override {
            writeSingerPackage(m_files, "one", "1");
            writeSingerPackage(m_files, "two", "2");
            inference::EnvironmentOptions options;
            options.packageRoots = {m_files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
            options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
            const auto host = inference::Environment::create(options);
            ASSERT_TRUE(host) << (host ? "" : host.error().formatChain());
            m_environment = *host;
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
            const auto mediaResources = media::Resources::create(std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "media" / "magic.mgc");
            ASSERT_TRUE(mediaResources);
            const auto created = arch_diffsinger::create({m_environment, *mediaResources, std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization"});
            ASSERT_TRUE(created) << (created ? "" : created.error().formatChain());
            m_implementation = *created;
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        arch::ArchitecturePtr m_implementation;
    };

    TEST_F(SingerDiscoveryTest, MissingModelsDoNotPreventDiscoveryAndVersionsHaveDistinctStableIdentities) {
        ASSERT_EQ(m_environment->packages().size(), 2U);
        const auto singers = m_implementation->singers({});
        ASSERT_TRUE(singers);
        ASSERT_EQ(singers->size(), 4U);
        EXPECT_TRUE(m_environment->unit().runtimeServices(ds::InferenceDriverPlugin::IID).empty());
        const auto *diffsinger = dynamic_cast<const arch_diffsinger::DiffSinger *>(m_implementation.get());
        ASSERT_NE(diffsinger, nullptr);
        std::set<std::string> ids;
        for (const auto &singer : *singers) {
            SCOPED_TRACE(singer.id);
            EXPECT_TRUE(ids.insert(singer.id).second);
            EXPECT_EQ(singer.arch, "diffsinger");
            EXPECT_EQ(singer.defaultLanguage, "eng");
            EXPECT_TRUE(singer.archSpecificInfo.isMember("speakers"));
            EXPECT_FALSE(singer.archSpecificInfo.isMember("hasDuration"));
            EXPECT_FALSE(singer.archSpecificInfo.isMember("languageCapabilities"));
            const auto group = m_implementation->group({singer.group, {}});
            ASSERT_TRUE(group);
            ASSERT_TRUE(*group);
            ASSERT_EQ((**group).languages.size(), 1U);
            EXPECT_EQ((**group).languages.at("eng").name, "English");
            EXPECT_TRUE((**group).languages.at("eng").defaultLyric.empty());
            EXPECT_EQ((**group).durationMode, arch::DurationMode::Skip);
            const auto *record = diffsinger->catalog().find(singer.id);
            ASSERT_NE(record, nullptr);
            EXPECT_TRUE(record->reservedPhonemes.empty());
            EXPECT_EQ(record->package.id(), "example/singers");
            EXPECT_EQ(record->spec->locator().category(), "singer");
            EXPECT_FALSE(record->package.version().isEmpty());
            ASSERT_TRUE(record->languages.contains("eng"));
            EXPECT_EQ(record->languages.at("eng").depth, inference::LanguageDepth::Pronunciation);
            for (const auto &group : runtime::InferenceCacheGroups) {
                if (!group.id.starts_with("diffsinger-")) continue;
                const auto pool = m_environment->inferences(group.id);
                ASSERT_TRUE(pool);
                EXPECT_EQ((*pool)->residentCount(), 0U);
            }
            EXPECT_FALSE(std::filesystem::exists(record->package.path() / "missing-acoustic.onnx"));
            const auto single = m_implementation->singer({singer.id, {"zh-CN"}});
            ASSERT_TRUE(single);
            ASSERT_TRUE(*single);
            EXPECT_EQ((**single).id, singer.id);
            EXPECT_EQ((**single).name, "歌手");
            const auto japanese = m_implementation->group({singer.group, {"ja-JP"}});
            ASSERT_TRUE(japanese);
            ASSERT_TRUE(*japanese);
            EXPECT_EQ((**japanese).languages.at("eng").name, "英語");
        }

        EXPECT_EQ(ids, (std::set<std::string>{"example/singers=1.0:singer/first", "example/singers=1.0:singer/second", "example/singers=2.0:singer/first", "example/singers=2.0:singer/second"}));
        const auto repeated = arch_diffsinger::SingerCatalog::discover(m_environment);
        ASSERT_TRUE(repeated);
        EXPECT_EQ((*repeated)->singers().size(), singers->size());
        for (const auto &singer : *singers) {
            ASSERT_NE((*repeated)->find(singer.id), nullptr);
            EXPECT_EQ((*repeated)->find(singer.id)->info.group, singer.group);
        }
    }

    TEST_F(SingerDiscoveryTest, GroupsReflectAvailableStagesAndSharedEffectivePipelines) {
        const auto groups = m_implementation->groups({});
        ASSERT_TRUE(groups);
        ASSERT_EQ(groups->size(), 2U);
        for (const auto &group : *groups) {
            EXPECT_FALSE(group.mixable);
            ASSERT_EQ(group.languages.size(), 1U);
            EXPECT_EQ(group.languages.at("eng").pronunciationMode, arch::PronunciationMode::Full);
            EXPECT_EQ(group.languages.at("eng").phonemeMode, arch::PhonemeMode::Skip);
            EXPECT_EQ(group.durationMode, arch::DurationMode::Skip);
            EXPECT_TRUE(group.archSpecificInfo.isObject());
            EXPECT_TRUE(group.archSpecificInfo.empty());
            EXPECT_EQ(group.defaultArchExtra["steps"].asInt(), 20);
            EXPECT_EQ(group.defaultArchExtra["depth"].asDouble(), 1);
            EXPECT_TRUE(std::holds_alternative<arch::DirectParameter>(group.parameterPipeline.at("pitch")));
            EXPECT_TRUE(group.parameterPipeline.contains("gender"));
            EXPECT_TRUE(group.parameterPipeline.contains("velocity"));
            EXPECT_TRUE(group.parameterPipeline.contains("tone_shift"));
            EXPECT_FALSE(group.parameterPipeline.contains("expressiveness"));
            const auto members = m_implementation->groupSingers({group.id, {}});
            ASSERT_TRUE(members);
            ASSERT_EQ(members->size(), 2U);
            EXPECT_NE((*members)[0].id, (*members)[1].id);
            for (const auto &singer : *members) {
                EXPECT_EQ(singer.group, group.id);
            }
            const auto single = m_implementation->group({group.id, {}});
            ASSERT_TRUE(single);
            ASSERT_TRUE(*single);
            EXPECT_EQ((**single).id, group.id);
        }
    }

    TEST_F(SingerDiscoveryTest, PredictiveStageDeclarationsExposeDurationAndRangeRetakesWithoutLoadingModels) {
        TestFiles predictive;
        writeSingerPackage(predictive, "predictive", "1", true);
        inference::EnvironmentOptions options;
        options.packageRoots = {predictive.path()};
        const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
        options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
        options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
        options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
        options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
        const auto host = inference::Environment::create(options);
        ASSERT_TRUE(host) << (host ? "" : host.error().formatChain());
        ASSERT_TRUE((*host)->problems().empty()) << ((*host)->problems().empty() ? "" : (*host)->problems().front().error.formatChain());
        const auto catalog = arch_diffsinger::SingerCatalog::discover(*host);
        ASSERT_TRUE(catalog) << (catalog ? "" : catalog.error().formatChain());
        ASSERT_EQ((*catalog)->singers().size(), 2U);
        ASSERT_EQ((*catalog)->groups().size(), 1U);
        const auto &group = (*catalog)->groups().begin()->second.info;
        EXPECT_EQ(group.durationMode, arch::DurationMode::Full);
        ASSERT_TRUE(std::holds_alternative<arch::IndirectParameter>(group.parameterPipeline.at("pitch")));
        const auto &pitch = std::get<arch::IndirectParameter>(group.parameterPipeline.at("pitch"));
        EXPECT_EQ(pitch.retakeMode, arch::ParameterRetakeMode::Range);
        EXPECT_EQ(pitch.dependsOn, (std::vector<std::string>{"expressiveness"}));
        ASSERT_TRUE(std::holds_alternative<arch::IndirectParameter>(group.parameterPipeline.at("energy")));
        const auto &energy = std::get<arch::IndirectParameter>(group.parameterPipeline.at("energy"));
        EXPECT_EQ(energy.retakeMode, arch::ParameterRetakeMode::Range);
        EXPECT_EQ(energy.dependsOn, (std::vector<std::string>{"pitch", "tone_shift"}));
        EXPECT_TRUE(std::holds_alternative<arch::DirectParameter>(group.parameterPipeline.at("expressiveness")));
        EXPECT_TRUE((*host)->unit().runtimeServices(ds::InferenceDriverPlugin::IID).empty());
        for (const auto &[id, singer] : (*catalog)->singers()) {
            SCOPED_TRACE(id);
            ASSERT_TRUE(singer.languages.contains("eng"));
            for (const auto &group : runtime::InferenceCacheGroups) {
                if (!group.id.starts_with("diffsinger-")) continue;
                const auto pool = (*host)->inferences(group.id);
                ASSERT_TRUE(pool);
                EXPECT_EQ((*pool)->residentCount(), 0U);
            }
            EXPECT_EQ(singer.info.defaultLanguage, "eng");
            for (const auto stage : {"duration", "pitch", "variance"}) {
                EXPECT_FALSE(std::filesystem::exists(singer.package.path() / (std::string("missing-") + stage + "-encoder.onnx")));
                EXPECT_FALSE(std::filesystem::exists(singer.package.path() / (std::string("missing-") + stage + "-predictor.onnx")));
            }
        }
    }

    TEST_F(SingerDiscoveryTest, LanguageCoverageResolvesPhonemesInEveryStage) {
        struct Case {
            std::string_view name;
            std::string_view acousticPhonemes;
            std::string_view predictivePhonemes;
            std::string_view exports;
            bool accepted;
        };
        const Case cases[] = {
            {"qualified", R"({"eng/a":0})", R"({"eng/a":0})", R"({"phonemes":["a"]})", true},
            {"bare", R"({"a":0})", R"({"a":0})", R"({"phonemes":["a"]})", true},
            {"mixed", R"({"eng/a":0})", R"({"a":0})", R"({"phonemes":["a"]})", true},
            {"mixed reverse", R"({"a":0})", R"({"eng/a":0})", R"({"phonemes":["a"]})", true},
            {"wrong language", R"({"cmn/a":0})", R"({"cmn/a":0})", R"({"phonemes":["a"]})", false},
            {"disjoint stages", R"({"eng/a":0})", R"({"eng/b":0})", R"({"phonemes":["a","b"]})", false},
            {"bare markers", R"({"SP":0,"AP":1})", R"({"SP":0,"AP":1})", R"({"phonemes":["SP","AP"]})", true},
            {"qualified silence", R"({"eng/SP":0})", R"({"eng/SP":0})", R"({"phonemes":["SP"]})", false},
            {"qualified breath", R"({"eng/AP":0})", R"({"eng/AP":0})", R"({"phonemes":["AP"]})", false},
            {"partial coverage", R"({"eng/a":0})", R"({"a":0})", R"({"phonemes":["a","b"]})", true},
            {"open inventory", R"({"eng/b":0})", R"({"eng/b":0})", R"({"phonemes":["a"],"openSet":true})", true},
            {"unknown inventory", R"({"eng/b":0})", R"({"eng/b":0})", R"({"phonemes":[]})", true},
        };
        for (const auto &testCase : cases) {
            SCOPED_TRACE(testCase.name);
            TestFiles files;
            writeSingerPackage(files, "coverage", "1", true);
            files.write("coverage/phonemes.json", testCase.predictivePhonemes);
            files.write("coverage/acoustic-phonemes.json", testCase.acousticPhonemes);
            files.write("coverage/acoustic.json", R"({"interface":"org.openvpi.dsinfer.inference.Acoustic","variant":"onnx","level":1,
                "exports":{"varianceControls":["energy"],"transitionControls":["gender","velocity"]},
                "configuration":{"model":"${root}/missing-acoustic.onnx","phonemes":"acoustic-phonemes.json","parameters":["energy","gender","velocity"]}})");
            files.write("coverage/eng.json", R"({"interface":"org.openvpi.wolf.linguist.WolfLinguist","variant":"wolf","level":1,
                "language":"eng","scheme":"test","exports":)" + std::string(testCase.exports) + R"(,
                "configuration":{},"imports":[{"role":"linguist/g2p","ref":":inference/g2p"}]})");
            inference::EnvironmentOptions options;
            options.packageRoots = {files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
            options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
            options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
            const auto host = inference::Environment::create(options);
            ASSERT_TRUE(host) << (host ? "" : host.error().formatChain());
            ASSERT_TRUE((*host)->problems().empty()) << ((*host)->problems().empty() ? "" : (*host)->problems().front().error.formatChain());
            const auto catalog = arch_diffsinger::SingerCatalog::discover(*host);
            ASSERT_TRUE(catalog) << (catalog ? "" : catalog.error().formatChain());
            ASSERT_EQ((*catalog)->singers().size(), 2U);
            for (const auto &[id, singer] : (*catalog)->singers()) {
                SCOPED_TRACE(id);
                EXPECT_EQ(singer.languages.contains("eng"), testCase.accepted);
                EXPECT_EQ(singer.info.defaultLanguage, testCase.accepted ? "eng" : "");
                EXPECT_EQ((*catalog)->groups().at(singer.info.group).info.languages.contains("eng"), testCase.accepted);
            }
        }
    }

    TEST_F(SingerDiscoveryTest, MediaUsesRawBytesAndNegotiationWhileAbsentOptionalResourcesAreEmpty) {
        const auto singers = m_implementation->singers({});
        ASSERT_TRUE(singers);
        ASSERT_FALSE(singers->empty());
        arch::SingerMediaQuery query{{singers->front().id, {}}, {{"IMAGE/GIF"}, {"DATA"}}};
        const auto avatar = m_implementation->avatar(query);
        ASSERT_TRUE(avatar);
        ASSERT_TRUE(avatar->starts_with("data:image/gif;base64,"));
        const std::string_view encoded(*avatar);
        const auto payload = encoded.substr(encoded.find(',') + 1);
        const codecutil::Base64DecodingView decoded(payload);
        EXPECT_EQ(std::string(decoded.begin(), decoded.end()), std::string(gif, sizeof(gif) - 1));
        const auto background = m_implementation->background(query);
        ASSERT_TRUE(background);
        EXPECT_TRUE(background->empty());
        const auto demo = m_implementation->demoAudio(query);
    ASSERT_TRUE(demo) << (demo ? "" : demo.error().asError().formatChain());
        EXPECT_TRUE(demo->empty());
        query.media.acceptableFormats = {"image/png"};
        const auto format = m_implementation->avatar(query);
        ASSERT_FALSE(format);
        EXPECT_NE(format.error().get_if<arch::InvalidInputError>(), nullptr);
        query.media.acceptableFormats.clear();
        query.media.acceptableSchemes = {"https"};
        const auto scheme = m_implementation->avatar(query);
        ASSERT_FALSE(scheme);
        EXPECT_NE(scheme.error().get_if<arch::InvalidInputError>(), nullptr);
        query.media.acceptableSchemes.clear();
        query.singer.display.acceptLanguage = "zh-CN";
        const auto missingLocalizedResource = m_implementation->avatar(query);
        ASSERT_FALSE(missingLocalizedResource);
        EXPECT_NE(missingLocalizedResource.error().get_if<arch::OperationError>(), nullptr);
    }

    TEST_F(SingerDiscoveryTest, ConcurrentReadersUseTheCommittedSingerSnapshot) {
        std::atomic<unsigned> failures = 0;
        std::vector<std::jthread> workers;
        for (unsigned i = 0; i < 8; ++i) {
            workers.emplace_back([&] {
                for (unsigned j = 0; j < 16; ++j) {
                    const auto singers = m_implementation->singers({j % 2 ? "zh" : "ja"});
                    if (!singers || singers->size() != 4U) {
                        ++failures;
                    }
                    const auto groups = m_implementation->groups({j % 2 ? "en" : "ja"});
                    if (!groups || groups->size() != 2U) {
                        ++failures;
                        continue;
                    }
                    for (const auto &group : *groups) {
                        const auto language = group.languages.find("eng");
                        if (language == group.languages.end() || language->second.name != (j % 2 ? "English" : "英語")) {
                            ++failures;
                        }
                    }
                }
            });
        }

        workers.clear();
        EXPECT_EQ(failures.load(), 0U);
    }

    TEST_F(SingerDiscoveryTest, MovingAnInstallationDoesNotChangeSingerOrGroupIdentities) {
        TestFiles relocated;
        writeSingerPackage(relocated, "moved", "1");
        inference::EnvironmentOptions options;
        options.packageRoots = {relocated.path()};
        const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
        options.pluginPaths = arch_diffsinger::pluginPaths(plugins);
        options.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
        options.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
        options.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
        const auto host = inference::Environment::create(options);
        ASSERT_TRUE(host);
        const auto catalog = arch_diffsinger::SingerCatalog::discover(*host);
        ASSERT_TRUE(catalog);
        ASSERT_EQ((*catalog)->singers().size(), 2U);
        for (const auto &[id, singer] : (*catalog)->singers()) {
            const auto original = m_implementation->singer({id, {}});
            ASSERT_TRUE(original);
            ASSERT_TRUE(*original);
            EXPECT_EQ((**original).group, singer.info.group);
        }
    }
}
