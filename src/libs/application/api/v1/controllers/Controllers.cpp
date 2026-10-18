#include "Controllers.h"

#include <dsspapiv1/ArchitectureDispatch.h>
#include <dsspapiv1/DiscoveryController.h>
#include <dsspapiv1/ExtractionDispatch.h>

namespace dssp::api_v1 {
    namespace {
        constexpr dto::ApiVersion apiVersion{1.0, 0.0};
    }

    void ApplicationController::info(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        const dto::ApplicationInfoResponse info{{apiVersion}};
        cb(http::jsonResponse(toJson(info), http::requestLog(*req)));
    }

    void CatalogController::architectures(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        std::vector<dto::ArchitectureMetadata> result;
        for (const auto &implementation : m_architectures->entries()) {
            auto metadata = implementation->metadata(detail::catalogOptions(*req));
            if (!metadata) {
                cb(architectureProblemResponse(metadata.error(), http::requestLog(*req)));
                return;
            }
            result.push_back(toDto(std::move(*metadata)));
        }

        cb(http::jsonResponse(toJson(result), http::requestLog(*req)));
    }

    void CatalogController::architecture(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(*req), &arch::Architecture::metadata, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void CatalogController::singers(const drogon::HttpRequestPtr &req, http::Callback &&cb) const {
        std::vector<dto::SingerInfo> result;
        for (const auto &implementation : m_architectures->entries()) {
            auto singers = implementation->singers(detail::catalogOptions(*req));
            if (!singers) {
                cb(architectureProblemResponse(singers.error(), http::requestLog(*req)));
                return;
            }
            for (auto &singer : *singers) {
                result.push_back(toDto(std::move(singer)));
            }
        }

        cb(http::jsonResponse(toJson(result), http::requestLog(*req)));
    }

    void CatalogController::archSingers(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(*req), &arch::Architecture::singers, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void CatalogController::singer(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(singer), detail::catalogOptions(*req)}, &arch::Architecture::singer, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void CatalogController::avatar(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::avatar, [](std::string url) { return dto::SingerAvatarResponse{std::move(url)}; });
    }

    void CatalogController::background(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::background, [](std::string url) { return dto::SingerBackgroundResponse{std::move(url)}; });
    }

    void CatalogController::demoAudio(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string singer) const {
        detail::catalogMedia(*m_architectures, req, cb, id, std::move(singer), &arch::Architecture::demoAudio);
    }

    void CatalogController::groups(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id) const {
        detail::catalog(*m_architectures, cb, id, detail::catalogOptions(*req), &arch::Architecture::groups, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void CatalogController::group(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string group) const {
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(group), detail::catalogOptions(*req)}, &arch::Architecture::group, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void CatalogController::groupSingers(const drogon::HttpRequestPtr &req, http::Callback &&cb, std::string id, std::string group) const {
        detail::catalog(*m_architectures, cb, id, arch::ResourceQuery{std::move(group), detail::catalogOptions(*req)}, &arch::Architecture::groupSingers, detail::DtoPresenter{}, http::requestLog(*req));
    }

    void SynthesisController::envTag(const drogon::HttpRequestPtr &request, http::Callback &&callback) const {
        auto input = request->as<Decoded<dto::EnvTagRequest>>();
        if (!input) {
            callback(http::problemResponse(input.error(), http::requestLog(*request)));
            return;
        }

        const auto selected = m_architectures->find(input->context.arch);
        if (!selected) {
            callback(detail::unknownProcessArchitecture(input->context.arch, http::requestLog(*request)));
            return;
        }

        detail::respond(selected->envTag(toArchitecture(std::move(*input))), callback, [](std::string tag) { return dto::EnvTagResponse{std::move(tag)}; }, http::requestLog(*request));
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
        detail::extractionCatalog(m_service->extractors(req->getHeader("accept-language")), cb, http::requestLog(*req));
    }

    void ExtractionController::noteExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Note, id, request->getHeader("accept-language")), callback, http::requestLog(*request));
    }

    void ExtractionController::tempoExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Tempo, id, request->getHeader("accept-language")), callback, http::requestLog(*request));
    }

    void ExtractionController::pitchExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Pitch, id, request->getHeader("accept-language")), callback, http::requestLog(*request));
    }

    void ExtractionController::separationExtractor(const drogon::HttpRequestPtr &request, http::Callback &&callback, std::string id) const {
        detail::extractionCatalog(m_service->extractor(extraction::Kind::Separation, id, request->getHeader("accept-language")), callback, http::requestLog(*request), toSeparationDto);
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
        app.registerController(std::make_shared<DiscoveryController>());
        app.registerController(std::make_shared<ApplicationController>());
        app.registerController(std::make_shared<CatalogController>(architectures));
        app.registerController(std::make_shared<SynthesisController>(lifetime, architectures, heartbeatInterval));
        app.registerController(std::make_shared<ExtractionController>(lifetime, extraction, heartbeatInterval));
    }

}
