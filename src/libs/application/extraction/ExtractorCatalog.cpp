#include "ExtractorCatalog.h"

#include <cmath>
#include <exception>

#include <stdcorelib/support/logging.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsspinference/Display.h>
#include <dsspinference/ResourceIdentity.h>
#include <dsspinference/ParallelismPolicy.h>

namespace dssp::extraction {
    namespace {
        namespace Note = otter::Api::Note::L1;
        namespace F0 = otter::Api::F0::L1;
        namespace Sep = srtsepinfer::Api::Separation::L1;
        namespace Beat = srtbeatinfer::Api::Beat::L1;
        stdc::LogCategory log("dssp.extraction.ExtractorCatalog");

        template <class Schema, class Create>
        std::expected<ExtractorRecord, inference::ResourceError> read(srt::InferenceSpec &spec, Kind kind, inference::InferencePool &pool, Create create) {
            const auto *schema = dynamic_cast<const Schema *>(spec.exports());
            if (!schema || !spec.configuration() || schema->interfaceId() != spec.interfaceId() || schema->level() != spec.level() || schema->variant() != spec.variant() || schema->sampleRate <= 0 || schema->channelCount != 1 || !std::isfinite(schema->maxSegmentDuration) || schema->maxSegmentDuration < 0) {
                return std::unexpected{inference::ResourceError("Analyzer has an unsupported typed contract, format or duration limit")};
            }

            ExtractorRecord record;
            record.kind = kind;
            record.id = inference::resourceReference(spec);
            record.name = spec.name();
            record.package = spec.package();
            record.spec = &spec;
            record.sampleRate = schema->sampleRate;
            record.maximumDuration = schema->maxSegmentDuration;
            record.pool = &pool;
            if constexpr (requires { schema->interval; }) {
                if (!std::isfinite(schema->interval) || schema->interval <= 0 || !std::isfinite(1.0 / schema->interval)) {
                    return std::unexpected{inference::ResourceError("F0 analyzer has an invalid output interval")};
                }
                record.outputInterval = schema->interval;
            }

            inference::InferenceDefinition definition;
            definition.key = record.id + "/default";
            definition.label = record.id;
            definition.packages = {record.package};
            definition.parallelSafe = inference::permitsParallelExecution(spec);
            definition.create = [&spec, create]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                auto created = inference::nativeCall("Create extraction executive", [&] { return create(spec); });
                if (!created) return std::unexpected{std::move(created.error())};
                return std::move(*created);
            };

            auto resource = pool.bind(std::move(definition));
            if (!resource) return std::unexpected{inference::ResourceError("Cannot bind analyzer resource", resource.error().clone())};
            record.resource = std::move(*resource);
            return record;
        }

        std::expected<ExtractorRecord, inference::ResourceError> readSeparation(srt::InferenceSpec &spec, inference::InferencePool &pool) {
            const auto *schema = dynamic_cast<const Sep::SeparationSchema *>(spec.exports());
            if (!schema || !spec.configuration() || schema->interfaceId() != spec.interfaceId() || schema->level() != spec.level() || schema->variant() != spec.variant() || schema->sampleRate <= 0 || schema->channelCount <= 0 || schema->tracks.empty() || !std::isfinite(schema->maxSegmentDuration) || schema->maxSegmentDuration != 0) return std::unexpected{inference::ResourceError("Separator has an unsupported typed audio or duration contract")};
            ExtractorRecord record;
            record.kind = Kind::Separation;
            record.id = inference::resourceReference(spec);
            record.name = spec.name();
            record.package = spec.package();
            record.spec = &spec;
            record.sampleRate = schema->sampleRate;
            record.channelCount = schema->channelCount;
            record.tracks = schema->tracks;
            record.pool = &pool;
            inference::InferenceDefinition definition;
            definition.key = record.id + "/default";
            definition.label = record.id;
            definition.packages = {record.package};
            definition.parallelSafe = inference::permitsParallelExecution(spec);
            definition.create = [&spec]() -> std::expected<std::unique_ptr<srt::InferenceExecutive>, inference::ExecutionError> {
                auto created = inference::nativeCall("srtsepinfer createSeparator", [&] { return Sep::createSeparator(spec); });
                if (!created) return std::unexpected{std::move(created.error())};
                return std::move(*created);
            };

            auto resource = pool.bind(std::move(definition));
            if (!resource) return std::unexpected{inference::ResourceError("Cannot bind separator resource", resource.error().clone())};
            record.resource = std::move(*resource);
            return record;
        }

        CatalogResult<ExtractorInfo> present(const ExtractorRecord &record, std::string_view acceptLanguage) {
            auto name = inference::localize(record.name, acceptLanguage, record.id);
            if (!name) return fail<CatalogErrors>(OperationError("Cannot localize extractor " + record.id, name.error().clone()));
            ExtractorInfo result{record.id, std::move(*name), static_cast<std::uint32_t>(record.sampleRate)};
            for (const auto &track : record.tracks) {
                auto description = inference::localize(track.description, acceptLanguage, track.id);
                if (!description) return fail<CatalogErrors>(OperationError("Cannot localize separation track " + track.id, description.error().clone()));
                result.tracks.push_back({track.id, std::move(*description)});
            }

            return result;
        }
    }

    std::expected<std::shared_ptr<const ExtractorCatalog>, inference::InitializationError> ExtractorCatalog::discover(std::shared_ptr<inference::Environment> environment) {
        if (!environment) return std::unexpected{inference::InitializationError("Extraction requires an inference environment")};
        auto catalog = std::shared_ptr<ExtractorCatalog>(new ExtractorCatalog(std::move(environment)));
        auto notePool = catalog->m_environment->inferences("extraction-note");
        auto pitchPool = catalog->m_environment->inferences("extraction-pitch");
        auto separationPool = catalog->m_environment->inferences("extraction-separation");
        auto tempoPool = catalog->m_environment->inferences("extraction-tempo");
        if (!notePool) return std::unexpected{std::move(notePool.error())};
        if (!pitchPool) return std::unexpected{std::move(pitchPool.error())};
        if (!separationPool) return std::unexpected{std::move(separationPool.error())};
        if (!tempoPool) return std::unexpected{std::move(tempoPool.error())};
        try {
            auto *category = dynamic_cast<srt::InferenceCategory *>(catalog->m_environment->unit().category(srt::InferenceCategory::NAME));
            if (!category) return std::unexpected{inference::InitializationError("Synthrt inference category is unavailable")};
            std::size_t notes = 0, pitches = 0, tempos = 0, separations = 0, rejected = 0;
            for (auto *spec : category->inferences()) {
                if (spec->variant() == "stub" || spec->level() != 1) continue;
                std::expected<ExtractorRecord, inference::ResourceError> record = std::unexpected{inference::ResourceError("Unsupported analysis contract")};
                if (spec->interfaceId() == Note::API_INTERFACE) {
                    record = read<Note::NoteSchema>(*spec, Kind::Note, **notePool, Note::createAnalyzer);
                } else if (spec->interfaceId() == F0::API_INTERFACE) {
                    record = read<F0::F0Schema>(*spec, Kind::Pitch, **pitchPool, F0::createAnalyzer);
                } else if (spec->interfaceId() == Sep::API_INTERFACE) {
                    record = readSeparation(*spec, **separationPool);
                } else if (spec->interfaceId() == Beat::API_INTERFACE) {
                    const auto *schema = dynamic_cast<const Beat::BeatSchema *>(spec->exports());
                    if (schema && schema->maxSegmentDuration == 0 && schema->downbeats)
                        record = read<Beat::BeatSchema>(*spec, Kind::Tempo, **tempoPool, Beat::createBeatTracker);
                } else {
                    continue;
                }

                if (!record) {
                    ++rejected;
                    log.stdcWarning("Skipping extractor %1: %2", inference::resourceReference(*spec), record.error().formatChain());
                    continue;
                }
                notes += record->kind == Kind::Note;
                pitches += record->kind == Kind::Pitch;
                tempos += record->kind == Kind::Tempo;
                separations += record->kind == Kind::Separation;
                log.stdcTrace("Registered extractor %1 rate=%2 maximum_seconds=%3 output_interval=%4", record->id, record->sampleRate, record->maximumDuration, record->outputInterval);
                const auto id = record->id;
                catalog->m_records.emplace(id, std::move(*record));
            }

            log.stdcInfo("Extractor catalogue ready: %1 note extractors, %2 pitch extractors, %3 tempo extractors, %4 separation extractors, %5 rejected declarations", notes, pitches, tempos, separations, rejected);
            return std::shared_ptr<const ExtractorCatalog>(std::move(catalog));
        } catch (const std::exception &error) {
            return std::unexpected{inference::InitializationError("Cannot enumerate Otter analyzers", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"otter/synthrt", "enumerate analyzers", "exception"}))};
        }
    }

    const ExtractorRecord *ExtractorCatalog::find(Kind kind, std::string_view id) const {
        const auto found = m_records.find(id);
        return found != m_records.end() && found->second.kind == kind ? &found->second : nullptr;
    }

    CatalogResult<ExtractorInfo> ExtractorCatalog::metadata(Kind kind, std::string_view id, std::string_view acceptLanguage) const {
        const auto *record = find(kind, id);
        if (!record) return fail<CatalogErrors>(ResourceNotFoundError("Extractor does not exist for the requested type: " + std::string(id)));
        return present(*record, acceptLanguage);
    }

    CatalogResult<ExtractorList> ExtractorCatalog::list(std::string_view acceptLanguage) const {
        ExtractorList result;
        for (const auto &[id, record] : m_records) {
            auto info = present(record, acceptLanguage);
            if (!info) return std::unexpected{std::move(info.error())};
            if (record.kind == Kind::Note) result.note.push_back(std::move(*info));
            else if (record.kind == Kind::Pitch) result.pitch.push_back(std::move(*info));
            else if (record.kind == Kind::Tempo) result.tempo.push_back(std::move(*info));
            else if (record.kind == Kind::Separation) result.separation.push_back(std::move(*info));
        }

        return result;
    }
}
