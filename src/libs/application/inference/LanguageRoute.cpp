#include "LanguageRoute.h"

#include <exception>

#include <synthrt/Core/ContribImportBinding.h>
#include <wolf/Api/Inferences/G2P/1/G2PApiL1.h>
#include <wolf/Api/Inferences/Onset/1/OnsetApiL1.h>
#include <wolf/Api/Inferences/S2P/1/S2PApiL1.h>
#include <wolf/Api/Linguists/Linguist/1/LinguistApiL1.h>

namespace dssp::inference {
    namespace {
        template <class Exports, class Options>
        std::expected<std::optional<srt::ContribImport>, ResourceError> stage(const srt::ContribSpec &linguist, std::string_view role, bool required) {
            auto imported = linguist.findImport(role);
            if (!imported) {
                if (required) {
                    return std::unexpected{ResourceError("Missing language stage " + std::string(role))};
                }
                return std::optional<srt::ContribImport>{};
            }

            const auto *binding = imported->binding();
            if (!binding || binding->state() != srt::ContribImportBinding::Active) {
                return std::unexpected{ResourceError("Unresolved language stage " + std::string(role))};
            }

            const auto &target = binding->target();
            const auto *exports = dynamic_cast<const Exports *>(target.exports());
            const auto *options = dynamic_cast<const Options *>(imported->options());
            if (!exports || !options || target.locator().category() != "inference" || target.interfaceId() != exports->interfaceId() || target.variant() != exports->variant() || target.level() != exports->level()) {
                return std::unexpected{ResourceError("Unsupported language stage contract for " + std::string(role))};
            }

            return imported;
        }
    }

    std::expected<LanguageRoute, ResourceError> readLanguageRoute(const srt::SingerSpec &singer, std::string_view handle) {
        namespace G2p = wolf::Api::G2P::L1;
        namespace S2p = wolf::Api::S2P::L1;
        namespace Onset = wolf::Api::Onset::L1;
        namespace Linguist = wolf::Api::Linguist::L1;
        try {
            const auto *pipeline = Linguist::WolfPipelineExtension::from(singer);
            if (!pipeline || !pipeline->locate(handle) || !pipeline->binding(handle)) {
                return std::unexpected{ResourceError("Singer has no resolved Wolf language metadata for " + std::string(handle))};
            }

            const auto role = singer.languages().find(std::string(handle));
            if (role == singer.languages().end()) {
                return std::unexpected{ResourceError("Singer does not declare language " + std::string(handle))};
            }

            const auto imported = singer.findImport(role->second);
            if (!imported || !imported->binding() || imported->binding()->state() != srt::ContribImportBinding::Active) {
                return std::unexpected{ResourceError("Singer language has no active binding: " + std::string(handle))};
            }

            const auto &target = imported->binding()->target();
            const auto *spec = dynamic_cast<const wolf::LinguistSpec *>(&target);
            if (!spec || target.interfaceId() != Linguist::API_INTERFACE || target.variant() != Linguist::API_VARIANT || target.level() != Linguist::API_LEVEL) {
                return std::unexpected{ResourceError("Unsupported linguist composition for " + std::string(handle))};
            }

            auto g2p = stage<G2p::G2PExports, G2p::G2PImportOptions>(target, Linguist::ROLE_G2P, true);
            auto s2p = stage<S2p::S2PExports, S2p::S2PImportOptions>(target, Linguist::ROLE_S2P, false);
            auto onset = stage<Onset::OnsetExports, Onset::OnsetImportOptions>(target, Linguist::ROLE_ONSET, false);
            if (!g2p) {
                return std::unexpected{std::move(g2p.error())};
            }
            if (!s2p) {
                return std::unexpected{std::move(s2p.error())};
            }
            if (!onset) {
                return std::unexpected{std::move(onset.error())};
            }

            LanguageRoute result;
            result.spec = spec;
            result.binding = *pipeline->binding(handle);
            result.g2p = std::move(*g2p);
            result.s2p = std::move(*s2p);
            result.onset = std::move(*onset);
            switch (pipeline->maxDepth(handle)) {
                case Linguist::Depth::Pronunciation: result.depth = LanguageDepth::Pronunciation; break;
                case Linguist::Depth::Phonemes: result.depth = LanguageDepth::Phonemes; break;
                case Linguist::Depth::Onsets: result.depth = LanguageDepth::Onsets; break;
                default: return std::unexpected{ResourceError("Unsupported Wolf language depth for " + std::string(handle))};
            }

            return result;
        } catch (const std::exception &error) {
            return std::unexpected{ResourceError("Cannot inspect native language bindings", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"synthrt/Wolf", "inspect language imports", "exception"}))};
        }
    }
}
