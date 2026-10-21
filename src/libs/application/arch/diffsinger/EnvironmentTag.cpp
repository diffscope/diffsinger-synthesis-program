#include "EnvironmentTag.h"

#include <exception>
#include <map>
#include <set>
#include <utility>

#include <synthrt/Core/ContribImportBinding.h>

#include <dssparchdiffsinger/SynthesisContext.h>
#include <dsspinference/ResourceIdentity.h>

namespace dssp::arch_diffsinger {
    namespace {
        std::expected<void, arch::OperationError> collectPackages(const inference::Environment &environment, const srt::ContribSpec &spec, std::set<std::string> &visited, std::map<std::string, srt::JsonValue> &packages) {
            if (!visited.insert(inference::resourceId(spec)).second) {
                return {};
            }

            const auto package = spec.package();
            const auto key = package.id() + '=' + package.version().toString();
            const auto *hash = environment.packageHash(package);
            if (!hash) {
                return std::unexpected{arch::OperationError("Installed content hash is unavailable for package " + key)};
            }
            packages.emplace(key, *hash);
            for (const auto &imported : spec.imports()) {
                if (imported.binding() && imported.binding()->state() == srt::ContribImportBinding::Active) {
                    if (auto collected = collectPackages(environment, imported.binding()->target(), visited, packages); !collected) {
                        return collected;
                    }
                }
            }
            return {};
        }
    }

    std::expected<std::string, arch::EnvironmentErrors> synthesisEnvironmentTag(const SingerCatalog &catalog, const arch::MultiSingerContext &input) {
        auto context = synthesisContext(catalog, input);
        if (!context) {
            if (const auto *invalid = context.error().get_if<arch::InvalidInputError>()) {
                return fail<arch::EnvironmentErrors>(*invalid);
            }
            return fail<arch::EnvironmentErrors>(arch::OperationError("Cannot resolve the synthesis environment", context.error().asError().clone()));
        }

        try {
            srt::JsonArray sources;
            std::set<std::string> visited;
            std::map<std::string, srt::JsonValue> packages;
            for (const auto &source : context->sources) {
                if (auto collected = collectPackages(*catalog.environment(), *source.singer->spec, visited, packages); !collected) {
                    return fail<arch::EnvironmentErrors>(std::move(collected.error()));
                }
                srt::JsonObject mappings;
                for (const auto &[stage, record] : source.singer->stages) {
                    srt::JsonObject mapping;
                    for (const auto &[logical, target] : record.speakerMapping) {
                        mapping.emplace(logical, target);
                    }
                    mappings.emplace(stage, std::move(mapping));
                }
                sources.push_back(srt::JsonObject{{"singer", source.singer->info.id}, {"speaker", source.speaker}, {"group", source.singer->info.group}, {"speakerMappings", std::move(mappings)}});
            }

            srt::JsonObject packageHashes;
            for (auto &[id, value] : packages) {
                packageHashes.emplace(id, std::move(value));
            }

            const srt::JsonValue effective(srt::JsonObject{
                {"contract", "dssp.diffsinger.synthesis.1;pitch=128;normalized-controls;seconds;native-retake-ranges"},
                {"programVersion", DSSP_PROGRAM_VERSION}, {"synthrtRevision", DSSP_SYNTHRT_REVISION}, {"wolfRevision", DSSP_WOLF_REVISION},
                {"steps", srt::JsonObject{{"pitch", context->options.steps.pitch}, {"variance", context->options.steps.variance}, {"acoustic", context->options.steps.acoustic}}},
                {"depth", static_cast<double>(context->options.depth)},
                {"sources", std::move(sources)}, {"packages", std::move(packageHashes)}
            });
            const auto digest = inference::fingerprint(effective.toJson());
            if (!digest) {
                return fail<arch::EnvironmentErrors>(arch::OperationError("Cannot compute the synthesis environment tag", digest.error().clone()));
            }

            return "ds1." + *digest;
        } catch (const std::exception &error) {
            return fail<arch::EnvironmentErrors>(arch::OperationError("Cannot read the synthesis environment resource graph", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"synthrt metadata", "serialize effective synthesis environment", "exception"})));
        }
    }
}
