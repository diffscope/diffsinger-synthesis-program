#ifndef DSSP_TESTS_TESTARCHITECTURE_H
#define DSSP_TESTS_TESTARCHITECTURE_H

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dssparch/ArchitectureCatalog.h>

namespace dssp::tests {

    class TestArchitecture final : public arch::Architecture {
    public:
        explicit TestArchitecture(std::string id = "diffsinger") : m_id(std::move(id)) {}
        std::string_view id() const noexcept override { return m_id; }
        arch::CatalogResult<arch::ArchitectureMetadata> metadata(const arch::CatalogOptions &) const override { return arch::ArchitectureMetadata{m_id, "test architecture", {}, {}}; }
        arch::CatalogResult<std::vector<arch::SingerInfo>> singers(const arch::CatalogOptions &) const override { return std::vector<arch::SingerInfo>{}; }

        arch::CatalogResult<std::optional<arch::SingerInfo>> singer(const arch::ResourceQuery &query) const override { return onSinger ? onSinger(query) : arch::CatalogResult<std::optional<arch::SingerInfo>>{std::optional<arch::SingerInfo>{}}; }
        arch::CatalogResult<std::string> avatar(const arch::SingerMediaQuery &query) const override { return onAvatar ? onAvatar(query) : arch::CatalogResult<std::string>{std::string{}}; }
        arch::CatalogResult<std::string> background(const arch::SingerMediaQuery &query) const override { return onBackground ? onBackground(query) : arch::CatalogResult<std::string>{std::string{}}; }
        arch::CatalogResult<std::vector<arch::SingerDemoAudio>> demoAudio(const arch::SingerMediaQuery &query) const override { return onDemoAudio ? onDemoAudio(query) : arch::CatalogResult<std::vector<arch::SingerDemoAudio>>{std::vector<arch::SingerDemoAudio>{}}; }

        arch::CatalogResult<std::vector<arch::GroupMetadata>> groups(const arch::CatalogOptions &options) const override { return onGroups ? onGroups(options) : arch::CatalogResult<std::vector<arch::GroupMetadata>>{std::vector<arch::GroupMetadata>{}}; }
        arch::CatalogResult<std::optional<arch::GroupMetadata>> group(const arch::ResourceQuery &query) const override { return onGroup ? onGroup(query) : arch::CatalogResult<std::optional<arch::GroupMetadata>>{std::optional<arch::GroupMetadata>{}}; }
        arch::CatalogResult<std::vector<arch::SingerInfo>> groupSingers(const arch::ResourceQuery &) const override { return std::vector<arch::SingerInfo>{}; }

        std::expected<std::string, arch::EnvironmentErrors> envTag(arch::MultiSingerContext) const override { return std::string{}; }

        void pronunciation(arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> completion) const override {
            if (onPronunciation) {
                onPronunciation(std::move(input), stop, std::move(completion));
                return;
            }
            complete(stop, std::move(completion));
        }

        void phoneme(arch::PhonemeInput, std::stop_token stop, arch::Completion<arch::PhonemeOutput> completion) const override { complete(stop, std::move(completion)); }
        void duration(arch::DurationInput, std::stop_token stop, arch::Completion<arch::DurationOutput> completion) const override { complete(stop, std::move(completion)); }
        void parameter(arch::ParameterInput, std::stop_token stop, arch::Completion<arch::ParameterOutput> completion) const override { complete(stop, std::move(completion)); }
        void audio(arch::AudioInput, std::stop_token stop, arch::Completion<arch::AudioOutput> completion) const override { complete(stop, std::move(completion)); }

        std::function<arch::CatalogResult<std::optional<arch::SingerInfo>>(const arch::ResourceQuery &)> onSinger;
        std::function<arch::CatalogResult<std::string>(const arch::SingerMediaQuery &)> onAvatar;
        std::function<arch::CatalogResult<std::string>(const arch::SingerMediaQuery &)> onBackground;
        std::function<arch::CatalogResult<std::vector<arch::SingerDemoAudio>>(const arch::SingerMediaQuery &)> onDemoAudio;
        std::function<arch::CatalogResult<std::vector<arch::GroupMetadata>>(const arch::CatalogOptions &)> onGroups;
        std::function<arch::CatalogResult<std::optional<arch::GroupMetadata>>(const arch::ResourceQuery &)> onGroup;
        std::function<void(arch::PronunciationInput, std::stop_token, arch::Completion<arch::PronunciationOutput>)> onPronunciation;

    private:
        template <class Output>
        static void complete(std::stop_token stop, arch::Completion<Output> completion) {
            if (stop.stop_requested()) {
                completion(fail<arch::SynthesisErrors>(arch::CancelledError("Test request was cancelled")));
            } else {
                completion(Output{});
            }
        }

        const std::string m_id;
    };

    inline std::shared_ptr<const arch::ArchitectureCatalog> testCatalog(std::vector<arch::ArchitecturePtr> entries = {std::make_shared<TestArchitecture>()}) {
        auto catalog = arch::ArchitectureCatalog::create(std::move(entries));
        EXPECT_TRUE(catalog);
        return catalog ? std::make_shared<const arch::ArchitectureCatalog>(std::move(*catalog)) : nullptr;
    }

}

#endif // DSSP_TESTS_TESTARCHITECTURE_H
