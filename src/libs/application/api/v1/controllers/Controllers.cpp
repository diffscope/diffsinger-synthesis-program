#include "Controllers.h"

#include <dsspapiv1/ArchitectureDispatch.h>
#include <dsspapiv1/DiscoveryController.h>
#include <dsspapiv1/ExtractionDispatch.h>

namespace dssp::api_v1 {
    namespace {
        constexpr dto::ApiVersion apiVersion{1.0, 0.0};
    }

    void ApplicationController::info(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        const auto context = m_lifetime->responseContext(*req);
        const dto::ApplicationInfoResponse info{{apiVersion}};
        cb(http::jsonResponse(toJson(info), context));
    }

    void CatalogController::architectures(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        const auto context = m_lifetime->responseContext(*req);
        std::vector<dto::ArchitectureMetadata> result;
        for (const auto &implementation : m_architectures->entries()) {
            auto metadata = implementation->metadata(detail::catalogOptions(context));
            if (!metadata) {
                cb(architectureProblemResponse(metadata.error(), context));
                return;
            }
            result.push_back(toDto(std::move(*metadata)));
        }

        cb(http::jsonResponse(toJson(result), context));
    }

    void CatalogController::architecture(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(context), &arch::Architecture::metadata, detail::DtoPresenter{}, context);
    }

    void CatalogController::singers(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        const auto context = m_lifetime->responseContext(*req);
        std::vector<dto::SingerInfo> result;
        for (const auto &implementation : m_architectures->entries()) {
            auto singers = implementation->singers(detail::catalogOptions(context));
            if (!singers) {
                cb(architectureProblemResponse(singers.error(), context));
                return;
            }
            for (auto &singer : *singers) {
                result.push_back(toDto(std::move(singer)));
            }
        }

        cb(http::jsonResponse(toJson(result), context));
    }

    void CatalogController::archSingers(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(context), &arch::Architecture::singers, detail::DtoPresenter{}, context);
    }

    void CatalogController::singer(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(singer), detail::catalogOptions(context)}, &arch::Architecture::singer, detail::DtoPresenter{}, context);
    }

    void CatalogController::avatar(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::avatar, context, [](std::string url) { return dto::SingerAvatarResponse{std::move(url)}; });
    }

    void CatalogController::background(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::background, context, [](std::string url) { return dto::SingerBackgroundResponse{std::move(url)}; });
    }

    void CatalogController::demoAudio(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::demoAudio, context);
    }

    void CatalogController::groups(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(context), &arch::Architecture::groups, detail::DtoPresenter{}, context);
    }

    void CatalogController::group(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string group) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(group), detail::catalogOptions(context)}, &arch::Architecture::group, detail::DtoPresenter{}, context);
    }

    void CatalogController::groupSingers(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string group) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(group), detail::catalogOptions(context)}, &arch::Architecture::groupSingers, detail::DtoPresenter{}, context);
    }

    void SynthesisController::envTag(const drogon::HttpRequestPtr &request, http::Callback &&callback) const {
        const auto context = m_lifetime->responseContext(*request);
        auto input = fromHttp<dto::EnvTagRequest>(*request, context.locale);
        if (!input) {
            callback(http::problemResponse(input.error(), context));
            return;
        }

        const auto selected = m_architectures->find(input->context.arch);
        if (!selected) {
            callback(detail::unknownProcessArchitecture(input->context.arch, context));
            return;
        }

        auto parameters = toArchitecture(std::move(*input));
        parameters.locale = context.locale;
        detail::respond(selected->envTag(std::move(parameters)), callback, [](std::string tag) { return dto::EnvTagResponse{std::move(tag)}; }, context);
    }

    drogon::Task<> SynthesisController::pronunciation(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::process<dto::PronunciationRequest, dto::PronunciationResponse, arch::PronunciationOutput>(request, std::move(callback), m_lifetime, m_architectures, &arch::Architecture::pronunciation, "pronunciation", m_heartbeatInterval);
    }

    drogon::Task<> SynthesisController::phoneme(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::process<dto::PhonemeRequest, dto::PhonemeResponse, arch::PhonemeOutput>(request, std::move(callback), m_lifetime, m_architectures, &arch::Architecture::phoneme, "phoneme", m_heartbeatInterval);
    }

    drogon::Task<> SynthesisController::duration(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::process<dto::DurationRequest, dto::DurationResponse, arch::DurationOutput>(request, std::move(callback), m_lifetime, m_architectures, &arch::Architecture::duration, "duration", m_heartbeatInterval);
    }

    drogon::Task<> SynthesisController::parameter(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::process<dto::ParameterRequest, dto::ParameterResponse, arch::ParameterOutput>(request, std::move(callback), m_lifetime, m_architectures, &arch::Architecture::parameter, "parameter", m_heartbeatInterval);
    }

    drogon::Task<> SynthesisController::audio(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::process<dto::AudioRequest, dto::AudioResponse, arch::AudioOutput>(request, std::move(callback), m_lifetime, m_architectures, &arch::Architecture::audio, "audio", m_heartbeatInterval);
    }

    void ExtractionController::extractors(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        const auto context = m_lifetime->responseContext(*req);
        detail::extractionCatalog(m_service->extractors(context.locale), cb, context);
    }

    void ExtractionController::noteExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        const auto context = m_lifetime->responseContext(*request);
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Note, id, context.locale), callback, context);
    }

    void ExtractionController::tempoExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        const auto context = m_lifetime->responseContext(*request);
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Tempo, id, context.locale), callback, context);
    }

    void ExtractionController::pitchExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        const auto context = m_lifetime->responseContext(*request);
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Pitch, id, context.locale), callback, context);
    }

    void ExtractionController::separationExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        const auto context = m_lifetime->responseContext(*request);
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Separation, id, context.locale), callback, context, toSeparationDto);
    }

    drogon::Task<> ExtractionController::note(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::extract<dto::NoteExtractionResponse, extraction::NoteOutput>(request, std::move(callback), m_lifetime, m_service, &extraction::Service::note, "note extraction", m_heartbeatInterval);
    }

    drogon::Task<> ExtractionController::pitch(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::extract<dto::PitchExtractionResponse, extraction::PitchOutput>(request, std::move(callback), m_lifetime, m_service, &extraction::Service::pitch, "pitch extraction", m_heartbeatInterval);
    }

    drogon::Task<> ExtractionController::tempo(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::extract<dto::TempoExtractionResponse, extraction::TempoOutput>(request, std::move(callback), m_lifetime, m_service, &extraction::Service::tempo, "tempo extraction", m_heartbeatInterval);
    }

    drogon::Task<> ExtractionController::separation(drogon::HttpRequestPtr request, http::Callback callback) const {
        co_await detail::extract<dto::SeparationExtractionResponse, extraction::SeparationOutput, dto::SeparationExtractionRequest>(request, std::move(callback), m_lifetime, m_service, &extraction::Service::separation, "audio separation", m_heartbeatInterval);
    }

    void registerControllers(drogon::HttpAppFramework &app, const std::shared_ptr<http::RequestLifetimeRegistry> &lifetime, const std::shared_ptr<const arch::ArchitectureCatalog> &architectures, const std::shared_ptr<extraction::Service> &extraction, std::chrono::seconds heartbeatInterval) {
        app.registerController(std::make_shared<DiscoveryController>(lifetime));
        app.registerController(std::make_shared<ApplicationController>(lifetime));
        app.registerController(std::make_shared<CatalogController>(architectures, lifetime));
        app.registerController(std::make_shared<SynthesisController>(lifetime, architectures, heartbeatInterval));
        app.registerController(std::make_shared<ExtractionController>(lifetime, extraction, heartbeatInterval));
    }

}
