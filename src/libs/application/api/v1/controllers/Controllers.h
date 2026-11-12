#ifndef DSSP_API_V1_CONTROLLERS_CONTROLLERS_H
#define DSSP_API_V1_CONTROLLERS_CONTROLLERS_H

#include <chrono>
#include <memory>
#include <utility>

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <dssparch/ArchitectureCatalog.h>
#include <dsspextraction/Service.h>
#include <dssphttp/RequestLifetime.h>
#include <dssphttp/Response.h>

namespace dssp::api_v1 {

    class ControllerSupport {
    public:
        explicit ControllerSupport(std::shared_ptr<http::RequestLifetimeRegistry> lifetime, std::chrono::seconds heartbeatInterval = std::chrono::seconds(10)) : m_lifetime(std::move(lifetime)), m_heartbeatInterval(heartbeatInterval) {}

    protected:
        std::shared_ptr<http::RequestLifetimeRegistry> m_lifetime;
        std::chrono::seconds m_heartbeatInterval;
    };

    class ApplicationController final : public drogon::HttpController<ApplicationController, false>, public ControllerSupport {
    public:
        explicit ApplicationController(std::shared_ptr<http::RequestLifetimeRegistry> lifetime = std::make_shared<http::RequestLifetimeRegistry>()) : ControllerSupport(std::move(lifetime)) {}

        METHOD_LIST_BEGIN
            ADD_METHOD_TO(ApplicationController::info, "/v1/info", drogon::Get);
        METHOD_LIST_END
        void info(const drogon::HttpRequestPtr &, http::Callback &&) const;
    };

    class CatalogController final : public drogon::HttpController<CatalogController, false>, public ControllerSupport {
    public:
        explicit CatalogController(std::shared_ptr<const arch::ArchitectureCatalog> architectures, std::shared_ptr<http::RequestLifetimeRegistry> lifetime = std::make_shared<http::RequestLifetimeRegistry>()) : ControllerSupport(std::move(lifetime)), m_architectures(std::move(architectures)) {}

        METHOD_LIST_BEGIN
            ADD_METHOD_TO(CatalogController::architectures, "/v1/arch", drogon::Get);
            ADD_METHOD_TO(CatalogController::architecture, "/v1/arch/{1}", drogon::Get);
            ADD_METHOD_TO(CatalogController::singers, "/v1/singer", drogon::Get);
            ADD_METHOD_TO(CatalogController::archSingers, "/v1/arch/{1}/singer", drogon::Get);
            // Drogon decodes the path before routing, including %2F inside a reference.
            // Capture package:category/contribution as one ID. Its final contribution
            // segment also keeps media routes distinct when a singer is named "avatar".
            ADD_METHOD_VIA_REGEX(CatalogController::singer, R"(/v1/arch/([^/]+)/singer/([^:]+:[^/]+/[^/]+))", drogon::Get);
            ADD_METHOD_VIA_REGEX(CatalogController::avatar, R"(/v1/arch/([^/]+)/singer/([^:]+:[^/]+/[^/]+)/avatar)", drogon::Get);
            ADD_METHOD_VIA_REGEX(CatalogController::background, R"(/v1/arch/([^/]+)/singer/([^:]+:[^/]+/[^/]+)/background)", drogon::Get);
            ADD_METHOD_VIA_REGEX(CatalogController::demoAudio, R"(/v1/arch/([^/]+)/singer/([^:]+:[^/]+/[^/]+)/demo-audio)", drogon::Get);
            ADD_METHOD_TO(CatalogController::groups, "/v1/arch/{1}/group", drogon::Get);
            ADD_METHOD_TO(CatalogController::group, "/v1/arch/{1}/group/{2}", drogon::Get);
            ADD_METHOD_TO(CatalogController::groupSingers, "/v1/arch/{1}/group/{2}/singer", drogon::Get);
        METHOD_LIST_END
        void architectures(const drogon::HttpRequestPtr &, http::Callback &&) const;
        void architecture(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch) const;
        void singers(const drogon::HttpRequestPtr &, http::Callback &&) const;
        void archSingers(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch) const;
        void singer(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string singer) const;
        void avatar(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string singer) const;
        void background(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string singer) const;
        void demoAudio(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string singer) const;
        void groups(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch) const;
        void group(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string group) const;
        void groupSingers(const drogon::HttpRequestPtr &, http::Callback &&, std::string arch, std::string group) const;

    private:
        const std::shared_ptr<const arch::ArchitectureCatalog> m_architectures;
    };

    class SynthesisController final : public drogon::HttpController<SynthesisController, false>, public ControllerSupport {
    public:
        SynthesisController(std::shared_ptr<http::RequestLifetimeRegistry> lifetime, std::shared_ptr<const arch::ArchitectureCatalog> architectures, std::chrono::seconds heartbeatInterval = std::chrono::seconds(10)) : ControllerSupport(std::move(lifetime), heartbeatInterval), m_architectures(std::move(architectures)) {}

        METHOD_LIST_BEGIN
            ADD_METHOD_TO(SynthesisController::envTag, "/v1/synth/env-tag", drogon::Post);
            ADD_METHOD_TO(SynthesisController::pronunciation, "/v1/synth/pronunciation", drogon::Post);
            ADD_METHOD_TO(SynthesisController::phoneme, "/v1/synth/phoneme", drogon::Post);
            ADD_METHOD_TO(SynthesisController::duration, "/v1/synth/duration", drogon::Post);
            ADD_METHOD_TO(SynthesisController::parameter, "/v1/synth/parameter", drogon::Post);
            ADD_METHOD_TO(SynthesisController::audio, "/v1/synth/audio", drogon::Post);
        METHOD_LIST_END
        void envTag(const drogon::HttpRequestPtr &, http::Callback &&) const;
        drogon::Task<> pronunciation(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> phoneme(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> duration(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> parameter(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> audio(drogon::HttpRequestPtr, http::Callback) const;

    private:
        const std::shared_ptr<const arch::ArchitectureCatalog> m_architectures;
    };

    class ExtractionController final : public drogon::HttpController<ExtractionController, false>, public ControllerSupport {
    public:
        ExtractionController(std::shared_ptr<http::RequestLifetimeRegistry> lifetime, std::shared_ptr<extraction::Service> service, std::chrono::seconds heartbeatInterval = std::chrono::seconds(10)) : ControllerSupport(std::move(lifetime), heartbeatInterval), m_service(std::move(service)) {}

        METHOD_LIST_BEGIN
            ADD_METHOD_TO(ExtractionController::extractors, "/v1/extractor", drogon::Get);
            ADD_METHOD_VIA_REGEX(ExtractionController::noteExtractor, R"(/v1/extractor/note/([^:]+:[^/]+/[^/]+))", drogon::Get);
            ADD_METHOD_VIA_REGEX(ExtractionController::tempoExtractor, R"(/v1/extractor/tempo/([^:]+:[^/]+/[^/]+))", drogon::Get);
            ADD_METHOD_VIA_REGEX(ExtractionController::pitchExtractor, R"(/v1/extractor/pitch/([^:]+:[^/]+/[^/]+))", drogon::Get);
            ADD_METHOD_VIA_REGEX(ExtractionController::separationExtractor, R"(/v1/extractor/separation/([^:]+:[^/]+/[^/]+))", drogon::Get);
            ADD_METHOD_TO(ExtractionController::note, "/v1/extract/note", drogon::Post);
            ADD_METHOD_TO(ExtractionController::tempo, "/v1/extract/tempo", drogon::Post);
            ADD_METHOD_TO(ExtractionController::pitch, "/v1/extract/pitch", drogon::Post);
            ADD_METHOD_TO(ExtractionController::separation, "/v1/extract/separation", drogon::Post);
        METHOD_LIST_END
        void extractors(const drogon::HttpRequestPtr &, http::Callback &&) const;
        void noteExtractor(const drogon::HttpRequestPtr &, http::Callback &&, std::string id) const;
        void tempoExtractor(const drogon::HttpRequestPtr &, http::Callback &&, std::string id) const;
        void pitchExtractor(const drogon::HttpRequestPtr &, http::Callback &&, std::string id) const;
        void separationExtractor(const drogon::HttpRequestPtr &, http::Callback &&, std::string id) const;
        drogon::Task<> note(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> tempo(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> pitch(drogon::HttpRequestPtr, http::Callback) const;
        drogon::Task<> separation(drogon::HttpRequestPtr, http::Callback) const;

    private:
        const std::shared_ptr<extraction::Service> m_service;
    };

    void registerControllers(drogon::HttpAppFramework &app, const std::shared_ptr<http::RequestLifetimeRegistry> &lifetime, const std::shared_ptr<const arch::ArchitectureCatalog> &architectures, const std::shared_ptr<extraction::Service> &extraction, std::chrono::seconds heartbeatInterval = std::chrono::seconds(10));

}

#endif // DSSP_API_V1_CONTROLLERS_CONTROLLERS_H
