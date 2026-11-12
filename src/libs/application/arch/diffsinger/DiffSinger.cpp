#include "DiffSinger.h"

#include <algorithm>
#include <exception>

#include <stdcorelib/path.h>
#include <stdcorelib/str.h>

#include <dssparchdiffsinger/EnvironmentTag.h>
#include <dssparchdiffsinger/Factory.h>
#include <dssparchdiffsinger/LanguagePrediction.h>
#include <dssparchdiffsinger/Metadata.h>
#include <dssparchdiffsinger/Synthesis.h>
#include <dsspinference/Display.h>
#include <dsspmedia/Resources.h>

namespace dssp::arch_diffsinger {
    namespace {
        template <class T, class Function>
        arch::CatalogResult<T> catalogueCall(Function &&function) {
            try {
                return function();
            } catch (const std::exception &error) {
                return fail<arch::CatalogErrors>(arch::OperationError("Cannot read singer metadata", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"metadata libraries", "read singer metadata", "exception"})));
            }
        }

        arch::CatalogResult<std::string> mediaUrl(const media::Resources &resources, const SingerRecord &record, const srt::DisplayText &path, const arch::SingerMediaQuery &query, media::Kind kind) {
            const auto selected = inference::localize(path, query.singer.display.acceptLanguage);
            if (!selected) {
                return fail<arch::CatalogErrors>(arch::OperationError("Cannot localize singer media path", selected.error().clone()));
            }
            if (selected->empty()) {
                return std::string{};
            }

            auto file = stdc::path::from_utf8(*selected);
            if (file.is_relative()) {
                file = record.package.path() / file;
            }

            auto resource = resources.read(file, kind);
            if (!resource) {
                return fail<arch::CatalogErrors>(arch::OperationError("Cannot read the declared singer media resource", resource.error().clone()));
            }
            if (!query.media.acceptableSchemes.empty() && std::ranges::none_of(query.media.acceptableSchemes, [](const auto &scheme) { return stdc::str::equals_insensitive(scheme, "data"); })) {
                return fail<arch::CatalogErrors>(arch::InvalidInputError(query.singer.display.locale.text("diffsinger.resource_scheme_unacceptable", "acceptableSchemes does not accept the resource scheme data")));
            }
            if (!query.media.acceptableFormats.empty() && std::ranges::none_of(query.media.acceptableFormats, [&](const auto &format) { return stdc::str::equals_insensitive(format, resource->mime); })) {
                return fail<arch::CatalogErrors>(arch::InvalidInputError(query.singer.display.locale.text("diffsinger.resource_format_unacceptable", "acceptableFormats does not accept the resource format {0}", resource->mime)));
            }

            return std::move(resource->dataUrl);
        }

        template <class Input, class Output, class Function>
        void synthesize(const std::shared_ptr<const SingerCatalog> &catalog, resource::TaskExecutor &tasks, Input input, std::stop_token stop, arch::Completion<Output> completion, Function function) {
            function(catalog, tasks, std::move(input), stop, std::move(completion));
        }
    }

    std::string_view DiffSinger::id() const noexcept {
        return Id;
    }

    arch::CatalogResult<arch::ArchitectureMetadata> DiffSinger::metadata(const arch::CatalogOptions &options) const {
        return catalogueCall<arch::ArchitectureMetadata>([&] { return architectureMetadata(options, *m_translations); });
    }

    arch::CatalogResult<std::vector<arch::SingerInfo>> DiffSinger::singers(const arch::CatalogOptions &options) const {
        return catalogueCall<std::vector<arch::SingerInfo>>([&]() -> arch::CatalogResult<std::vector<arch::SingerInfo>> {
            std::vector<arch::SingerInfo> result;
            result.reserve(m_catalog->singers().size());
            for (const auto &[id, singer] : m_catalog->singers()) {
                auto info = singerInfo(singer, options);
                if (!info) {
                    return std::unexpected{std::move(info.error())};
                }
                result.push_back(std::move(*info));
            }

            return result;
        });
    }

    arch::CatalogResult<std::optional<arch::SingerInfo>> DiffSinger::singer(const arch::ResourceQuery &query) const {
        return catalogueCall<std::optional<arch::SingerInfo>>([&]() -> arch::CatalogResult<std::optional<arch::SingerInfo>> {
            const auto *record = m_catalog->find(query.id);
            if (!record) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.display.locale.text("diffsinger.singer_not_found", "Singer does not exist: {0}", query.id)));
            }

            auto info = singerInfo(*record, query.display);
            if (!info) {
                return std::unexpected{std::move(info.error())};
            }

            return std::optional{std::move(*info)};
        });
    }

    arch::CatalogResult<std::string> DiffSinger::avatar(const arch::SingerMediaQuery &query) const {
        return catalogueCall<std::string>([&]() -> arch::CatalogResult<std::string> {
            const auto *record = m_catalog->find(query.singer.id);
            if (!record) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.singer.display.locale.text("diffsinger.singer_not_found", "Singer does not exist: {0}", query.singer.id)));
            }

            return mediaUrl(*m_media, *record, record->avatar, query, media::Kind::Image);
        });
    }

    arch::CatalogResult<std::string> DiffSinger::background(const arch::SingerMediaQuery &query) const {
        return catalogueCall<std::string>([&]() -> arch::CatalogResult<std::string> {
            const auto *record = m_catalog->find(query.singer.id);
            if (!record) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.singer.display.locale.text("diffsinger.singer_not_found", "Singer does not exist: {0}", query.singer.id)));
            }

            return mediaUrl(*m_media, *record, record->background, query, media::Kind::Image);
        });
    }

    arch::CatalogResult<std::vector<arch::SingerDemoAudio>> DiffSinger::demoAudio(const arch::SingerMediaQuery &query) const {
        return catalogueCall<std::vector<arch::SingerDemoAudio>>([&]() -> arch::CatalogResult<std::vector<arch::SingerDemoAudio>> {
            const auto *record = m_catalog->find(query.singer.id);
            if (!record) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.singer.display.locale.text("diffsinger.singer_not_found", "Singer does not exist: {0}", query.singer.id)));
            }

            auto url = mediaUrl(*m_media, *record, record->demoAudio, query, media::Kind::Audio);
            if (!url) {
                return std::unexpected{std::move(url.error())};
            }

            std::vector<arch::SingerDemoAudio> result;
            if (!url->empty()) {
                auto name = inference::localize(record->name, query.singer.display.acceptLanguage, record->info.id);
                if (!name) {
                    return fail<arch::CatalogErrors>(arch::OperationError("Cannot localize demo audio name", name.error().clone()));
                }
                result.push_back({std::move(*name), std::move(*url)});
            }

            return result;
        });
    }

    arch::CatalogResult<std::vector<arch::GroupMetadata>> DiffSinger::groups(const arch::CatalogOptions &options) const {
        return catalogueCall<std::vector<arch::GroupMetadata>>([&]() -> arch::CatalogResult<std::vector<arch::GroupMetadata>> {
            std::vector<arch::GroupMetadata> result;
            result.reserve(m_catalog->groups().size());
            for (const auto &[id, group] : m_catalog->groups()) {
                auto info = groupInfo(group, options);
                if (!info) {
                    return std::unexpected{std::move(info.error())};
                }
                result.push_back(std::move(*info));
            }

            return result;
        });
    }

    arch::CatalogResult<std::optional<arch::GroupMetadata>> DiffSinger::group(const arch::ResourceQuery &query) const {
        return catalogueCall<std::optional<arch::GroupMetadata>>([&]() -> arch::CatalogResult<std::optional<arch::GroupMetadata>> {
            const auto found = m_catalog->groups().find(query.id);
            if (found == m_catalog->groups().end()) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.display.locale.text("diffsinger.group_not_found", "Group does not exist: {0}", query.id)));
            }

            auto info = groupInfo(found->second, query.display);
            if (!info) {
                return std::unexpected{std::move(info.error())};
            }

            return std::optional{std::move(*info)};
        });
    }

    arch::CatalogResult<std::vector<arch::SingerInfo>> DiffSinger::groupSingers(const arch::ResourceQuery &query) const {
        return catalogueCall<std::vector<arch::SingerInfo>>([&]() -> arch::CatalogResult<std::vector<arch::SingerInfo>> {
            if (!m_catalog->groups().contains(query.id)) {
                return fail<arch::CatalogErrors>(arch::ResourceNotFoundError(query.display.locale.text("diffsinger.group_not_found", "Group does not exist: {0}", query.id)));
            }

            std::vector<arch::SingerInfo> result;
            for (const auto &[id, singer] : m_catalog->singers()) {
                if (singer.info.group == query.id) {
                    auto info = singerInfo(singer, query.display);
                    if (!info) {
                        return std::unexpected{std::move(info.error())};
                    }
                    result.push_back(std::move(*info));
                }
            }

            return result;
        });
    }

    std::expected<std::string, arch::EnvironmentErrors> DiffSinger::envTag(arch::MultiSingerContext context) const {
        return synthesisEnvironmentTag(*m_catalog, context);
    }

    void DiffSinger::pronunciation(arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> cb) const {
        predictPronunciation(m_catalog, std::move(input), stop, std::move(cb));
    }

    void DiffSinger::phoneme(arch::PhonemeInput input, std::stop_token stop, arch::Completion<arch::PhonemeOutput> cb) const {
        predictPhoneme(m_catalog, std::move(input), stop, std::move(cb));
    }

    void DiffSinger::duration(arch::DurationInput input, std::stop_token stop, arch::Completion<arch::DurationOutput> cb) const {
        synthesize(m_catalog, *m_tasks, std::move(input), stop, std::move(cb), synthesizeDuration);
    }

    void DiffSinger::parameter(arch::ParameterInput input, std::stop_token stop, arch::Completion<arch::ParameterOutput> cb) const {
        synthesize(m_catalog, *m_tasks, std::move(input), stop, std::move(cb), synthesizeParameters);
    }

    void DiffSinger::audio(arch::AudioInput input, std::stop_token stop, arch::Completion<arch::AudioOutput> cb) const {
        synthesize(m_catalog, *m_tasks, std::move(input), stop, std::move(cb), synthesizeAudio);
    }

}
