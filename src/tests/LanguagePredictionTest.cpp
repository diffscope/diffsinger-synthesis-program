#include <chrono>
#include <future>
#include <limits>

#include <gtest/gtest.h>

#include <dssparchdiffsinger/DiffSinger.h>
#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/LanguageResults.h>
#include <dssptests/TestLanguagePackage.h>

namespace dssp::tests {
    using namespace std::chrono_literals;

    class LanguagePredictionTest : public ::testing::Test {
    protected:
        void loadEnvironment(LanguagePackageOptions options = {}) {
            options.singer = true;
            writeLanguagePackage(m_files, "package", options);
            inference::EnvironmentOptions configuration;
            configuration.packageRoots = {m_files.path()};
            const std::filesystem::path plugins(DSSP_TEST_PLUGIN_DIR);
            configuration.pluginPaths = arch_diffsinger::pluginPaths(plugins);
            configuration.pluginPaths.push_back({"inference", plugins / "dsinfer" / "inferenceinterpreters"});
            configuration.pluginPaths.push_back({"inference", plugins / "wolf" / "inferenceinterpreters"});
            configuration.pluginPaths.push_back({"linguist", plugins / "wolf" / "linguistproviders"});
            const auto host = inference::Environment::create(configuration);
            ASSERT_TRUE(host) << (host ? "" : host.error().formatChain());
            m_environment = *host;
        }

        void initialize(LanguagePackageOptions options = {}) {
            loadEnvironment(options);
            ASSERT_NE(m_environment, nullptr);
            ASSERT_TRUE(m_environment->problems().empty()) << (m_environment->problems().empty() ? "" : m_environment->problems().front().error.formatChain());
            const auto resources = media::Resources::create(std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "media" / "magic.mgc");
            ASSERT_TRUE(resources);
            const auto created = arch_diffsinger::create({m_environment, *resources, std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization"});
            ASSERT_TRUE(created) << (created ? "" : created.error().formatChain());
            m_implementation = *created;
            const auto singers = m_implementation->singers({});
            ASSERT_TRUE(singers);
            ASSERT_EQ(singers->size(), 1U);
            m_context = {Json::Value(Json::objectValue), {singers->front().id, Json::Value(Json::objectValue)}};
        }

        void TearDown() override {
            if (m_environment) {
                m_environment->shutdown();
            }
        }

        template <class Output, class Submit>
        arch::SynthesisResult<Output> await(Submit submit) {
            const auto promise = std::make_shared<std::promise<arch::SynthesisResult<Output>>>();
            auto result = promise->get_future();
            submit([promise](auto output) { promise->set_value(std::move(output)); });
            const auto ready = result.wait_for(5s);
            EXPECT_EQ(ready, std::future_status::ready);
            if (ready != std::future_status::ready) {
                return fail<arch::SynthesisErrors>(arch::OperationError("Test timed out waiting for language prediction"));
            }
            return result.get();
        }

        arch::SynthesisResult<arch::PronunciationOutput> pronunciation(std::vector<arch::Lyric> notes) {
            return await<arch::PronunciationOutput>([&](auto callback) { m_implementation->pronunciation({m_context, std::move(notes)}, {}, std::move(callback)); });
        }

        arch::SynthesisResult<arch::PhonemeOutput> phoneme(std::vector<arch::PronunciationNote> notes) {
            return await<arch::PhonemeOutput>([&](auto callback) { m_implementation->phoneme({m_context, std::move(notes)}, {}, std::move(callback)); });
        }

        TestFiles m_files;
        std::shared_ptr<inference::Environment> m_environment;
        arch::ArchitecturePtr m_implementation;
        arch::SingleSingerContext m_context;
    };

    TEST_F(LanguagePredictionTest, RealG2pPreservesOrderCandidatesFallbackAndLiteralEditorCharacters) {
        initialize();
        ASSERT_NE(m_implementation, nullptr);
        for (const auto group : {"diffsinger-pronunciation", "diffsinger-phoneme", "diffsinger-onset"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }

        const auto result = pronunciation({{"read", ""}, {"unknown", "eng"}, {"foreign", "jpn"}, {"", "eng"}, {"SP", "eng"}, {"a+", "eng"}, {"-", "eng"}});
        ASSERT_TRUE(result) << (result ? "" : result.error().asError().formatChain());
        ASSERT_EQ(result->notes.size(), 7U);
        EXPECT_EQ(result->notes[0].pronunciation, "b a b i");
        EXPECT_EQ(result->notes[0].candidates, (std::vector<std::string>{"b a b i", "b i"}));
        EXPECT_EQ(result->notes[1].pronunciation, "unknown");
        EXPECT_EQ(result->notes[1].candidates, (std::vector<std::string>{"unknown"}));
        EXPECT_EQ(result->notes[2].pronunciation, "foreign");
        EXPECT_TRUE(result->notes[3].pronunciation.empty());
        EXPECT_TRUE(result->notes[3].candidates.empty());
        EXPECT_EQ(result->notes[4].candidates, (std::vector<std::string>{"SP"}));
        EXPECT_EQ(result->notes[5].pronunciation, "a+");
        EXPECT_EQ(result->notes[6].pronunciation, "-");
        const auto pronunciationPool = m_environment->inferences("diffsinger-pronunciation");
        ASSERT_TRUE(pronunciationPool);
        EXPECT_EQ((*pronunciationPool)->residentCount(), 1U);
        for (const auto group : {"diffsinger-phoneme", "diffsinger-onset"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(LanguagePredictionTest, RealS2pAndOnsetConvertFullInputsBeforeSlicingAndDoNotInvokeG2p) {
        initialize();
        ASSERT_NE(m_implementation, nullptr);
        const auto result = phoneme({{"b a b i", "eng", {}, {}}, {"b a b i", "eng", 1, {}}, {"b a b i", "eng", {}, -1}, {"b a b i", "eng", 0, 0}, {"b a b i", "eng", -100, 100}, {"AP", "missing", {}, {}}, {"word", "missing", {}, {}}, {"", "eng", {}, {}}});
        ASSERT_TRUE(result) << (result ? "" : result.error().asError().formatChain());
        ASSERT_EQ(result->notes.size(), 8U);
        const auto &full = result->notes[0].phonemes;
        ASSERT_EQ(full.size(), 4U);
        EXPECT_EQ(full[0].token, "b");
        EXPECT_TRUE(full[0].onset);
        EXPECT_FALSE(full[1].onset);
        EXPECT_TRUE(full[2].onset);
        EXPECT_FALSE(full[3].onset);
        ASSERT_EQ(result->notes[1].phonemes.size(), 2U);
        EXPECT_EQ(result->notes[1].phonemes[1].token, "i");
        ASSERT_EQ(result->notes[2].phonemes.size(), 2U);
        EXPECT_EQ(result->notes[2].phonemes[1].token, "a");
        EXPECT_TRUE(result->notes[3].phonemes.empty());
        EXPECT_EQ(result->notes[4].phonemes.size(), full.size());
        ASSERT_EQ(result->notes[5].phonemes.size(), 1U);
        EXPECT_EQ(result->notes[5].phonemes[0].token, "AP");
        EXPECT_TRUE(result->notes[5].phonemes[0].onset);
        EXPECT_TRUE(result->notes[6].phonemes.empty());
        EXPECT_TRUE(result->notes[7].phonemes.empty());
        const auto pool = m_environment->inferences("diffsinger-phoneme");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 1U);
        const auto onset = m_environment->inferences("diffsinger-onset");
        const auto pronunciation = m_environment->inferences("diffsinger-pronunciation");
        ASSERT_TRUE(onset);
        ASSERT_TRUE(pronunciation);
        EXPECT_EQ((*onset)->residentCount(), 1U);
        EXPECT_EQ((*pronunciation)->residentCount(), 0U);
    }

    TEST_F(LanguagePredictionTest, TokenOnlyAndMissingDictionaryEntriesAreNormalResults) {
        initialize({.onsets = false, .dictionaryS2p = true});
        ASSERT_NE(m_implementation, nullptr);
        const auto result = phoneme({{"test", "eng", {}, {}}, {"unlisted", "eng", {}, {}}, {"test", "eng", 1, {}}, {"test", "eng", -1, {}}});
        ASSERT_TRUE(result) << (result ? "" : result.error().asError().formatChain());
        ASSERT_EQ(result->notes.size(), 4U);
        ASSERT_EQ(result->notes[0].phonemes.size(), 4U);
        for (const auto &phone : result->notes[0].phonemes) {
            EXPECT_FALSE(phone.onset);
        }

        EXPECT_TRUE(result->notes[1].phonemes.empty());
        EXPECT_TRUE(result->notes[2].phonemes.empty());
        EXPECT_EQ(result->notes[3].phonemes.size(), 4U);
    }

    TEST_F(LanguagePredictionTest, PhonemesRemainIndependentOfNoteOrderBatchingAndPriorCalls) {
        initialize();
        ASSERT_NE(m_implementation, nullptr);
        const auto alone = phoneme({{"b a b i", "eng", {}, {}}});
        const auto mixed = phoneme({{"i", "eng", {}, {}}, {"b a b i", "eng", {}, {}}, {"b a", "eng", {}, {}}});
        const auto reordered = phoneme({{"b a", "eng", {}, {}}, {"b a b i", "eng", {}, {}}, {"i", "eng", {}, {}}});
        ASSERT_TRUE(alone);
        ASSERT_TRUE(mixed);
        ASSERT_TRUE(reordered);
        ASSERT_EQ(alone->notes.size(), 1U);
        ASSERT_EQ(mixed->notes.size(), 3U);
        ASSERT_EQ(reordered->notes.size(), 3U);
        const auto &expected = alone->notes[0].phonemes;
        for (const auto *actual : {&mixed->notes[1].phonemes, &reordered->notes[1].phonemes}) {
            ASSERT_EQ(actual->size(), expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ((*actual)[i].token, expected[i].token);
                EXPECT_EQ((*actual)[i].onset, expected[i].onset);
            }
        }
    }

    TEST_F(LanguagePredictionTest, EmptyNotesAndCancellationDoNotCreateExecutives) {
        initialize();
        ASSERT_NE(m_implementation, nullptr);
        const auto words = pronunciation({});
        const auto phones = phoneme({});
        ASSERT_TRUE(words);
        ASSERT_TRUE(phones);
        EXPECT_TRUE(words->notes.empty());
        EXPECT_TRUE(phones->notes.empty());
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = await<arch::PhonemeOutput>([&](auto callback) { m_implementation->phoneme({m_context, {{"b a", "eng", {}, {}}}}, stop.get_token(), std::move(callback)); });
        ASSERT_FALSE(cancelled);
        EXPECT_NE(cancelled.error().get_if<arch::CancelledError>(), nullptr);
        for (const auto group : {"diffsinger-pronunciation", "diffsinger-phoneme", "diffsinger-onset"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(LanguagePredictionTest, SingerWithoutLanguagesFallsBackWithoutLoadingExecutives) {
        initialize({.singerLanguages = false});
        ASSERT_NE(m_implementation, nullptr);
        const auto singers = m_implementation->singers({});
        ASSERT_TRUE(singers);
        ASSERT_EQ(singers->size(), 1U);
        EXPECT_TRUE(singers->front().defaultLanguage.empty());
        const auto groups = m_implementation->groups({});
        ASSERT_TRUE(groups);
        ASSERT_EQ(groups->size(), 1U);
        EXPECT_TRUE(groups->front().languages.empty());
        const auto result = pronunciation({{"read", ""}, {"read", "eng"}, {"foreign", "jpn"}});
        ASSERT_TRUE(result);
        ASSERT_EQ(result->notes.size(), 3U);
        for (std::size_t i = 0; i < 2; ++i) {
            EXPECT_EQ(result->notes[i].pronunciation, "read");
            EXPECT_EQ(result->notes[i].candidates, (std::vector<std::string>{"read"}));
        }

        EXPECT_EQ(result->notes[2].pronunciation, "foreign");
        EXPECT_EQ(result->notes[2].candidates, (std::vector<std::string>{"foreign"}));
        const auto phones = phoneme({{"read", "", {}, {}}, {"read", "eng", {}, {}}});
        ASSERT_TRUE(phones);
        ASSERT_EQ(phones->notes.size(), 2U);
        EXPECT_TRUE(phones->notes[0].phonemes.empty());
        EXPECT_TRUE(phones->notes[1].phonemes.empty());
        for (const auto group : {"diffsinger-pronunciation", "diffsinger-phoneme", "diffsinger-onset"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(LanguagePredictionTest, CustomReservedTokensAreNotExtendedWithSpAp) {
        initialize({.customReserved = true});
        ASSERT_NE(m_implementation, nullptr);
        const auto result = pronunciation({{"read", ""}, {"REST", ""}, {"read", "eng"}});
        ASSERT_TRUE(result);
        ASSERT_EQ(result->notes.size(), 3U);
        EXPECT_EQ(result->notes[0].pronunciation, "b a b i");
        EXPECT_EQ(result->notes[1].pronunciation, "REST");
        EXPECT_EQ(result->notes[2].pronunciation, "b a b i");
        const auto phones = phoneme({{"REST", "eng", {}, {}}, {"AP", "eng", {}, {}}});
        ASSERT_TRUE(phones);
        ASSERT_EQ(phones->notes.size(), 2U);
        ASSERT_EQ(phones->notes[0].phonemes.size(), 1U);
        EXPECT_TRUE(phones->notes[0].phonemes[0].onset);
        ASSERT_EQ(phones->notes[1].phonemes.size(), 1U);
        EXPECT_FALSE(phones->notes[1].phonemes[0].onset);
    }

    TEST_F(LanguagePredictionTest, UndeclaredReservedTokensDoNotBypassInference) {
        initialize({.noReserved = true});
        ASSERT_NE(m_implementation, nullptr);
        const auto result = phoneme({{"SP", "eng", {}, {}}, {"AP", "eng", {}, {}}});
        ASSERT_TRUE(result);
        ASSERT_EQ(result->notes.size(), 2U);
        for (const auto &note : result->notes) {
            ASSERT_EQ(note.phonemes.size(), 1U);
            EXPECT_FALSE(note.phonemes.front().onset);
        }

        const auto pool = m_environment->inferences("diffsinger-phoneme");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 1U);
    }

    TEST_F(LanguagePredictionTest, MissingDeclaredDictionaryRejectsThePackageWithADiagnostic) {
        loadEnvironment({.missingS2pDictionary = true});
        ASSERT_NE(m_environment, nullptr);
        EXPECT_TRUE(m_environment->packages().empty());
        ASSERT_EQ(m_environment->problems().size(), 1U);
        const auto &problem = m_environment->problems().front();
        EXPECT_EQ(problem.path, m_files.path() / "package");
        ASSERT_NE(problem.error.cause(), nullptr);
        auto cause = problem.error.cause();
        while (cause->cause()) {
            cause = cause->cause();
        }

        const auto *native = dynamic_cast<const ExternalError *>(cause.get());
        ASSERT_NE(native, nullptr);
        EXPECT_EQ(native->diagnostic().domain, srt::Error::category().name());
        EXPECT_EQ(native->diagnostic().code, srt::Error::FileNotFound);
        for (const auto group : {"diffsinger-pronunciation", "diffsinger-phoneme", "diffsinger-onset"}) {
            const auto pool = m_environment->inferences(group);
            ASSERT_TRUE(pool);
            EXPECT_EQ((*pool)->residentCount(), 0U);
        }
    }

    TEST_F(LanguagePredictionTest, UnavailableInferencePoolIsAnUnexpectedErrorAndContextValidationPrecedesBinding) {
        initialize({.dictionaryS2p = true});
        ASSERT_NE(m_implementation, nullptr);
        const auto pool = m_environment->inferences("diffsinger-phoneme");
        ASSERT_TRUE(pool);
        EXPECT_EQ((*pool)->residentCount(), 0U);
        (*pool)->shutdown();
        m_context.archExtra["depth"] = 2;
        const auto invalid = phoneme({{"test", "eng", {}, {}}});
        ASSERT_FALSE(invalid);
        EXPECT_NE(invalid.error().get_if<arch::InvalidInputError>(), nullptr);
        m_context.archExtra = Json::Value(Json::objectValue);
        const auto result = phoneme({{"test", "eng", {}, {}}});
        ASSERT_FALSE(result);
        const auto *error = result.error().get_if<arch::OperationError>();
        ASSERT_NE(error, nullptr);
        EXPECT_NE(error->cause(), nullptr);
        EXPECT_EQ((*pool)->residentCount(), 0U);
    }

    TEST(LanguageResults, ExpectedWordFailuresFallbackButRuntimeFailuresKeepNativeDiagnostics) {
        using Error = wolf::Api::G2p::L1::Error;
        wolf::Api::G2p::L1::WordOutput word;
        for (const auto error : {Error::InvalidInput, Error::PhonemeGenerationFailed}) {
            word.error = error;
            word.pronunciation = "undefined";
            const auto result = arch_diffsinger::translatePronunciation(word, "original");
            ASSERT_TRUE(result);
            EXPECT_EQ(result->pronunciation, "original");
            EXPECT_EQ(result->candidates, (std::vector<std::string>{"original"}));
        }

        for (const auto error : {Error::ModelInferenceFailed, Error::DriverUnavailable, Error::NotInitialized, Error::UnknownError}) {
            word.error = error;
            const auto result = arch_diffsinger::translatePronunciation(word, "original");
            ASSERT_FALSE(result);
            const auto *failure = result.error().get_if<arch::OperationError>();
            ASSERT_NE(failure, nullptr);
            EXPECT_NE(failure->cause(), nullptr);
        }
    }

    TEST(LanguageResults, CandidateFragmentsCollapseAndCopySkipFollowWolfContract) {
        namespace G2P = wolf::Api::G2p::L1;
        G2P::WordOutput word;
        word.pronunciation = "b a";
        word.candidates = {" b ", "a", ""};
        const auto normalized = arch_diffsinger::translatePronunciation(word, "lyric");
        ASSERT_TRUE(normalized);
        EXPECT_EQ(normalized->candidates, (std::vector<std::string>{"b a"}));
        word.mode = G2P::Mode::Copy;
        word.candidates.clear();
        const auto copied = arch_diffsinger::translatePronunciation(word, "lyric");
        ASSERT_TRUE(copied);
        EXPECT_TRUE(copied->candidates.empty());
        word.mode = G2P::Mode::Skip;
        const auto skipped = arch_diffsinger::translatePronunciation(word, "lyric");
        ASSERT_TRUE(skipped);
        EXPECT_TRUE(skipped->pronunciation.empty());
    }

    TEST(LanguageResults, SyllableSlicesPreservePreOnsetPhonesAndHandleExtremeEndpoints) {
        const std::vector<arch::Phoneme> phones{{"pre", false}, {"first", true}, {"tail", false}, {"second", true}, {"third", true}};
        const auto initial = arch_diffsinger::slicePhonemes(phones, {"", "", 0, 1});
        ASSERT_EQ(initial.size(), 3U);
        EXPECT_EQ(initial[0].token, "pre");
        EXPECT_FALSE(initial[0].onset);
        const auto last = arch_diffsinger::slicePhonemes(phones, {"", "", -1, {}});
        ASSERT_EQ(last.size(), 1U);
        EXPECT_EQ(last[0].token, "third");
        EXPECT_EQ(arch_diffsinger::slicePhonemes(phones, {"", "", std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}).size(), phones.size());
        EXPECT_TRUE(arch_diffsinger::slicePhonemes(phones, {"", "", 3, 1}).empty());
        EXPECT_TRUE(arch_diffsinger::slicePhonemes({}, {"", "", -1, 1}).empty());
    }
}
