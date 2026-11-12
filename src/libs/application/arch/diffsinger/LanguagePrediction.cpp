#include "LanguagePrediction.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <mutex>

#include <stdcorelib/support/logging.h>
#include <synthrt/Core/ContribImportBinding.h>
#include <wolf/Api/Inferences/G2P/1/G2PApiL1.h>
#include <wolf/Api/Inferences/Onset/1/OnsetApiL1.h>
#include <wolf/Api/Inferences/S2P/1/S2PApiL1.h>

#include <dssparchdiffsinger/LanguageResults.h>
#include <dssparchdiffsinger/SynthesisContext.h>
#include <dsspresource/TaskProgress.h>

namespace dssp::arch_diffsinger {
    namespace {
        namespace G2P = wolf::Api::G2P::L1;
        namespace S2P = wolf::Api::S2P::L1;
        namespace Onset = wolf::Api::Onset::L1;
        stdc::LogCategory log("dssp.arch_diffsinger.LanguagePrediction");

        arch::SynthesisErrors executionError(const inference::ExecutionErrors &error) {
            if (error.get_if<inference::CancelledError>()) {
                return arch::SynthesisErrors{arch::CancelledError("Language prediction was cancelled", error.asError().clone())};
            }
            return arch::SynthesisErrors{arch::OperationError("Language inference failed", error.asError().clone())};
        }

        arch::SynthesisResult<const SingerRecord *> contextSinger(const SingerCatalog &catalog, const arch::SingleSingerContext &context) {
            auto options = synthesisOptions(context.archExtra, context.locale);
            if (!options) {
                return std::unexpected{std::move(options.error())};
            }

            auto source = singerSource(catalog, context.singer, context.locale);
            if (!source) {
                return std::unexpected{std::move(source.error())};
            }

            return source->singer;
        }

        bool reserved(const SingerRecord &singer, const std::string &word) {
            return std::ranges::find(singer.reservedPhonemes, word) != singer.reservedPhonemes.end();
        }

        const inference::LanguageRoute *routeFor(const SingerRecord &singer, const std::string &language) {
            const auto found = singer.languages.find(language.empty() ? singer.info.defaultLanguage : language);
            return found == singer.languages.end() ? nullptr : &found->second;
        }

        template <class Executive, class InitArgs, class RuntimeOptions>
        std::expected<inference::InferenceHandle, inference::ExecutionError> bind(inference::InferencePool &pool, const srt::ContribImport &imported, const inference::LanguageRoute &route) {
            try {
                const auto *binding = imported.binding();
                if (!binding || binding->state() != srt::ContribImportBinding::Active) {
                    return std::unexpected{inference::ExecutionError("Language inference import is not active")};
                }

                auto options = std::make_shared<RuntimeOptions>(binding->target().variant());
                srt::JsonObject effective;
                if constexpr (requires { options->binding; }) {
                    options->binding = route.binding;
                    effective = {{"language", route.binding.language}, {"scheme", route.binding.scheme}};
                }

                return pool.bindImport<Executive, InitArgs>(imported, std::move(options), effective);
            } catch (const std::exception &error) {
                return std::unexpected{inference::ExecutionError("Cannot read language inference binding", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"wolf/synthrt", "read inference runtime options", "exception"}))};
            }
        }

        // Independent inference completions can arrive on different workers.
        // Publish exactly one reply, and cancel the remaining work after a fault.
        template <class Output>
        struct Request {
            Request(std::shared_ptr<const SingerCatalog> catalog, std::stop_token external, arch::Completion<Output> complete, Output output, std::size_t pending, resource::TaskProgress progress, std::size_t total)
                : catalog(std::move(catalog)), complete(std::move(complete)), output(std::move(output)), pending(pending), progress(std::move(progress)), total(total), cancellation(external, [source = stop]() mutable { source.request_stop(); }) {}
            std::shared_ptr<const SingerCatalog> catalog;

            arch::Completion<Output> complete;
            Output output;
            std::size_t pending;
            resource::TaskProgress progress;
            std::size_t total;
            std::size_t completed = 0;
            std::mutex mutex;
            bool done = false;
            std::stop_source stop;
            std::stop_callback<std::function<void()>> cancellation;

            template <class Note>
            void finish(const std::vector<std::size_t> &indices, arch::SynthesisResult<std::vector<Note>> result) {
                std::optional<arch::SynthesisResult<Output>> reply;
                std::optional<double> value;
                {
                    const std::lock_guard lock(mutex);
                    if (done) {
                        return;
                    }
                    if (stop.stop_requested()) {
                        reply.emplace(fail<arch::SynthesisErrors>(arch::CancelledError("Language prediction was cancelled")));
                    } else if (!result) {
                        reply.emplace(std::unexpected{std::move(result.error())});
                    } else {
                        for (std::size_t i = 0; i < indices.size(); ++i) {
                            output.notes[indices[i]] = std::move((*result)[i]);
                        }
                        completed += indices.size();
                        value = static_cast<double>(completed) / total;
                        if (!--pending) {
                            reply.emplace(std::move(output));
                        }
                    }
                    done = reply.has_value();
                }

                if (value) progress.advance(*value);
                if (reply) {
                    if (!*reply) {
                        stop.request_stop();
                    }

                    complete(std::move(*reply));
                }
            }
        };

        struct PronunciationBatch {
            const inference::LanguageRoute *route;
            inference::InferenceHandle handle;
            std::vector<std::size_t> indices;
            std::shared_ptr<G2P::G2PStartInput> input = std::make_shared<G2P::G2PStartInput>();
        };

        struct PhonemeTask {
            const inference::LanguageRoute *route;
            inference::InferenceHandle s2p;
            inference::InferenceHandle onset;
            std::size_t index;
            arch::PronunciationNote note;
        };

        using PhonemeRequest = Request<arch::PhonemeOutput>;

        void finishPhoneme(const std::shared_ptr<PhonemeRequest> &request, const PhonemeTask &task, std::vector<std::string> tokens, std::vector<bool> onsets) {
            std::vector<arch::Phoneme> phones;
            phones.reserve(tokens.size());
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                phones.push_back({std::move(tokens[i]), onsets[i]});
            }
            request->finish<arch::PhonemeNote>({task.index}, std::vector<arch::PhonemeNote>{{slicePhonemes(std::move(phones), task.note)}});
        }

        drogon::Task<> predictOnset(inference::InferencePool &pool, std::shared_ptr<PhonemeRequest> request, PhonemeTask task, std::vector<std::string> tokens) {
            if (tokens.empty() || !task.onset) {
                auto onsets = std::vector<bool>(tokens.size(), false);
                finishPhoneme(request, task, std::move(tokens), std::move(onsets));
                co_return;
            }

            auto input = std::make_shared<Onset::OnsetStartInput>();
            input->phonemes.push_back(std::move(tokens));
            const auto handle = task.onset;
            auto result = co_await pool.runCoro<Onset::OnsetExecutive>(handle, request->stop.get_token(), [input](auto &executive) { return executive.start(*input); }, request->progress);
            if (!result) {
                request->finish<arch::PhonemeNote>({task.index}, std::unexpected{executionError(result.error())});
            } else if (!*result || (*result)->onsets.size() != 1 || (*result)->onsets[0].size() != input->phonemes[0].size()) {
                request->finish<arch::PhonemeNote>({task.index}, fail<arch::SynthesisErrors>(arch::OperationError("Onset inference returned an incomplete result")));
            } else {
                finishPhoneme(request, task, std::move(input->phonemes[0]), std::move((*result)->onsets[0]));
            }
        }
    }

    void predictPronunciation(std::shared_ptr<const SingerCatalog> catalog, arch::PronunciationInput input, std::stop_token stop, arch::Completion<arch::PronunciationOutput> complete) {
        if (stop.stop_requested()) {
            complete(fail<arch::SynthesisErrors>(arch::CancelledError("Pronunciation request was cancelled")));
            return;
        }

        const auto singer = contextSinger(*catalog, input.context);
        if (!singer) {
            complete(std::unexpected{singer.error()});
            return;
        }

        const auto pool = catalog->environment()->inferences("diffsinger-pronunciation");
        if (!pool) {
            complete(fail<arch::SynthesisErrors>(arch::OperationError("Language cache group is unavailable", pool.error().clone())));
            return;
        }

        arch::PronunciationOutput output;
        output.notes.resize(input.notes.size());
        std::vector<PronunciationBatch> batches;
        std::map<const inference::LanguageRoute *, std::size_t> groups;
        std::size_t fallbackCount = 0;
        for (std::size_t i = 0; i < input.notes.size(); ++i) {
            const auto &note = input.notes[i];
            if (note.lyric.empty()) {
                continue;
            }

            const auto *route = routeFor(**singer, note.language);
            if (reserved(**singer, note.lyric)) {
                output.notes[i] = {note.lyric, {note.lyric}};
                continue;
            }
            if (!route || !route->g2p) {
                output.notes[i] = {note.lyric, {note.lyric}};
                ++fallbackCount;
                continue;
            }

            auto [group, inserted] = groups.emplace(route, batches.size());
            if (inserted) {
                auto handle = bind<G2P::G2PExecutive, G2P::G2PInitArgs, G2P::G2PRuntimeOptions>(**pool, *route->g2p, *route);
                if (!handle) {
                    complete(fail<arch::SynthesisErrors>(arch::OperationError("Cannot bind pronunciation inference", handle.error().clone())));
                    return;
                }
                batches.push_back({route, std::move(*handle), {}});
            }
            auto &batch = batches[group->second];
            batch.indices.push_back(i);
            batch.input->lyrics.push_back(note.lyric);
        }

        log.stdcTrace("Pronunciation singer=%1 notes=%2 batches=%3 unavailable_language_fallbacks=%4", (*singer)->info.id, input.notes.size(), batches.size(), fallbackCount);
        if (batches.empty()) {
            complete(stop.stop_requested() ? arch::SynthesisResult<arch::PronunciationOutput>{fail<arch::SynthesisErrors>(arch::CancelledError("Pronunciation request was cancelled"))} : arch::SynthesisResult<arch::PronunciationOutput>{std::move(output)});
            return;
        }

        std::size_t total = 0;
        for (const auto &batch : batches) total += batch.indices.size();
        const auto request = std::make_shared<Request<arch::PronunciationOutput>>(std::move(catalog), stop, std::move(complete), std::move(output), batches.size(), input.progress, total);
        for (auto &batch : batches) {
            const auto nativeInput = batch.input;
            const auto handle = batch.handle;
            drogon::async_run([pool = *pool, request, batch = std::move(batch), nativeInput, handle]() -> drogon::Task<> {
                auto result = co_await pool->runCoro<G2P::G2PExecutive>(handle, request->stop.get_token(), [nativeInput](auto &executive) { return executive.start(*nativeInput); }, request->progress);
                if (!result) {
                    request->finish<arch::Pronunciation>(batch.indices, std::unexpected{executionError(result.error())});
                    co_return;
                }
                if (!*result || (*result)->words.size() != batch.indices.size()) {
                    request->finish<arch::Pronunciation>(batch.indices, fail<arch::SynthesisErrors>(arch::OperationError("Pronunciation inference returned an incomplete result")));
                    co_return;
                }

                std::vector<arch::Pronunciation> notes;
                notes.reserve(batch.indices.size());
                std::size_t fallbackCount = 0;
                for (std::size_t i = 0; i < batch.indices.size(); ++i) {
                    const auto &word = (*result)->words[i];
                    auto converted = translatePronunciation(word, batch.input->lyrics[i]);
                    if (!converted) {
                        request->finish<arch::Pronunciation>(batch.indices, std::unexpected{std::move(converted.error())});
                        co_return;
                    }
                    fallbackCount += word.error != G2P::Error::None;
                    notes.push_back(std::move(*converted));
                }

                log.stdcTrace("Pronunciation language=%1 batch completed notes=%2 word_fallbacks=%3", batch.route->binding.language, notes.size(), fallbackCount);
                request->finish<arch::Pronunciation>(batch.indices, std::move(notes));
            });
        }
    }

    void predictPhoneme(std::shared_ptr<const SingerCatalog> catalog, arch::PhonemeInput input, std::stop_token stop, arch::Completion<arch::PhonemeOutput> complete) {
        if (stop.stop_requested()) {
            complete(fail<arch::SynthesisErrors>(arch::CancelledError("Phoneme request was cancelled")));
            return;
        }

        const auto singer = contextSinger(*catalog, input.context);
        if (!singer) {
            complete(std::unexpected{singer.error()});
            return;
        }

        const auto pool = catalog->environment()->inferences("diffsinger-phoneme");
        if (!pool) {
            complete(fail<arch::SynthesisErrors>(arch::OperationError("Language cache group is unavailable", pool.error().clone())));
            return;
        }

        arch::PhonemeOutput output;
        const auto onsetPool = catalog->environment()->inferences("diffsinger-onset");
        if (!onsetPool) {
            complete(fail<arch::SynthesisErrors>(arch::OperationError("Onset cache group is unavailable", onsetPool.error().clone())));
            return;
        }

        output.notes.resize(input.notes.size());
        std::vector<PhonemeTask> tasks;
        std::size_t fallbackCount = 0;
        for (std::size_t i = 0; i < input.notes.size(); ++i) {
            const auto &note = input.notes[i];
            if (note.pronunciation.empty()) {
                continue;
            }
            if (reserved(**singer, note.pronunciation)) {
                output.notes[i].phonemes = slicePhonemes({{note.pronunciation, true}}, note);
                continue;
            }

            const auto *route = routeFor(**singer, note.language);
            if (!route || !route->s2p) {
                ++fallbackCount;
                continue;
            }

            auto s2p = bind<S2P::S2PExecutive, S2P::S2PInitArgs, S2P::S2PRuntimeOptions>(**pool, *route->s2p, *route);
            if (!s2p) {
                complete(fail<arch::SynthesisErrors>(arch::OperationError("Cannot bind phoneme inference", s2p.error().clone())));
                return;
            }

            inference::InferenceHandle onset;
            if (route->onset) {
                auto handle = bind<Onset::OnsetExecutive, Onset::OnsetInitArgs, Onset::OnsetRuntimeOptions>(**onsetPool, *route->onset, *route);
                if (!handle) {
                    complete(fail<arch::SynthesisErrors>(arch::OperationError("Cannot bind onset inference", handle.error().clone())));
                    return;
                }
                onset = std::move(*handle);
            }
            tasks.push_back({route, std::move(*s2p), std::move(onset), i, note});
        }

        log.stdcTrace("Phoneme singer=%1 notes=%2 inference_items=%3 unavailable_language_fallbacks=%4", (*singer)->info.id, input.notes.size(), tasks.size(), fallbackCount);
        if (tasks.empty()) {
            complete(stop.stop_requested() ? arch::SynthesisResult<arch::PhonemeOutput>{fail<arch::SynthesisErrors>(arch::CancelledError("Phoneme request was cancelled"))} : arch::SynthesisResult<arch::PhonemeOutput>{std::move(output)});
            return;
        }

        const auto request = std::make_shared<PhonemeRequest>(std::move(catalog), stop, std::move(complete), std::move(output), tasks.size(), input.progress, tasks.size());
        for (auto &task : tasks) {
            auto nativeInput = std::make_shared<S2P::S2PStartInput>();
            nativeInput->pronunciations.push_back(task.note.pronunciation);
            const auto handle = task.s2p;
            drogon::async_run([pool = *pool, onsetPool = *onsetPool, request, task = std::move(task), nativeInput, handle]() mutable -> drogon::Task<> {
                auto result = co_await pool->runCoro<S2P::S2PExecutive>(handle, request->stop.get_token(), [nativeInput](auto &executive) { return executive.start(*nativeInput); }, request->progress);
                if (!result) {
                    request->finish<arch::PhonemeNote>({task.index}, std::unexpected{executionError(result.error())});
                } else if (!*result || (*result)->phonemes.size() != 1) {
                    request->finish<arch::PhonemeNote>({task.index}, fail<arch::SynthesisErrors>(arch::OperationError("Phoneme inference returned an incomplete result")));
                } else {
                    if ((*result)->phonemes[0].empty()) {
                        log.stdcTrace("Phoneme language=%1 conversion returned an empty sequence", task.route->binding.language);
                    }

                    co_await predictOnset(*onsetPool, request, std::move(task), std::move((*result)->phonemes[0]));
                }
            });
        }
    }
}
