#ifndef DSSP_EXTRACTION_SERVICE_H
#define DSSP_EXTRACTION_SERVICE_H

#include <memory>
#include <stop_token>
#include <string_view>

#include <dsspextraction/Types.h>
#include <dsspinference/Environment.h>
#include <dssplocalization/Context.h>

namespace dssp::extraction {
    class Service final {
    public:
        static std::expected<std::shared_ptr<Service>, inference::InitializationError> create(std::shared_ptr<inference::Environment>, double silenceThreshold = 0.02);
        ~Service();
        CatalogResult<ExtractorList> extractors(const localization::Context &locale = {}) const;
        CatalogResult<ExtractorInfo> extractor(Kind, std::string_view id, const localization::Context &locale = {}) const;
        void note(Input, std::stop_token, Completion<NoteOutput>) const;
        void pitch(Input, std::stop_token, Completion<PitchOutput>) const;
        void tempo(Input, std::stop_token, Completion<TempoOutput>) const;
        void separation(SeparationInput, std::stop_token, Completion<SeparationOutput>) const;
        // Call on the host thread while the inference environment is alive.
        void shutdown();

    private:
        struct Impl;
        explicit Service(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> m_impl;
    };
}

#endif // DSSP_EXTRACTION_SERVICE_H
