#ifndef DSSP_ARCHDIFFSINGER_SINGERCATALOG_H
#define DSSP_ARCHDIFFSINGER_SINGERCATALOG_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <synthrt/Core/ContribSpec.h>
#include <synthrt/SVS/SingerContrib.h>

#include <dssparch/Errors.h>
#include <dssparch/Types.h>
#include <dsspinference/Environment.h>
#include <dsspinference/LanguageRoute.h>

namespace dssp::arch_diffsinger {

    struct StageRecord {
        const srt::ContribSpec *spec = nullptr;
        const srt::ContribImportOptions *options = nullptr;
        bool usesSpeakers = false;
        std::vector<std::string> speakers;
        std::set<std::string> embeddedSpeakers;
        std::map<std::string, std::string> speakerMapping;
    };

    struct GroupRecord {
        arch::GroupMetadata info;
        // DisplayText stays immutable; names are selected for each HTTP response.
        std::map<std::string, srt::DisplayText> languageNames;
    };

    struct SingerRecord {
        srt::PackageHandle package;
        const srt::SingerSpec *spec = nullptr;
        arch::SingerInfo info;
        srt::DisplayText name;
        srt::DisplayText avatar;
        srt::DisplayText background;
        srt::DisplayText demoAudio;
        std::map<std::string, StageRecord> stages;
        std::map<std::string, inference::LanguageRoute> languages;
        std::vector<std::string> reservedPhonemes;
        std::vector<std::string> speakers;
        // Keep the model's exported order after applying the singer's import subset.
        std::vector<std::string> variancePredictions;
        bool technicallyMixable = false;
        bool permitsMixing = true;
    };

    // Only declaration data is retained. No pipeline/executive is created here.
    class SingerCatalog final {
    public:
        static std::expected<std::shared_ptr<const SingerCatalog>, inference::ResourceError> discover(std::shared_ptr<inference::Environment>);
        const std::map<std::string, SingerRecord> &singers() const noexcept { return m_singers; }
        const std::map<std::string, GroupRecord> &groups() const noexcept { return m_groups; }
        const SingerRecord *find(std::string_view id) const;
        const std::shared_ptr<inference::Environment> &environment() const noexcept { return m_environment; }

    private:
        explicit SingerCatalog(std::shared_ptr<inference::Environment> environment) : m_environment(std::move(environment)) {}

        // Retains the unit until all borrowed specs and package handles are gone.
        std::shared_ptr<inference::Environment> m_environment;
        std::map<std::string, SingerRecord> m_singers;
        std::map<std::string, GroupRecord> m_groups;
    };

    std::vector<std::string> commonSpeakers(const std::map<std::string, StageRecord> &);
    arch::CatalogResult<arch::SingerInfo> singerInfo(const SingerRecord &, const arch::CatalogOptions &);
    arch::CatalogResult<arch::GroupMetadata> groupInfo(const GroupRecord &, const arch::CatalogOptions &);
}

#endif // DSSP_ARCHDIFFSINGER_SINGERCATALOG_H
