#include "SynthesisScore.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace dssp::arch_diffsinger {
    namespace {
        bool reserved(const SingerRecord &singer, std::string_view token) {
            return std::ranges::find(singer.reservedPhonemes, token) != singer.reservedPhonemes.end();
        }

        template <class Phoneme>
        double startOf(const Phoneme &phone) {
            if constexpr (requires { phone.start; }) {
                return phone.start;
            }
            return 0;
        }

        template <class Note>
        arch::SynthesisResult<SynthesisScore> buildScore(const SingerRecord &singer, const arch::Piece<Note> &piece, const localization::Context &locale) {
            if (!std::isfinite(piece.pieceDuration) || piece.pieceDuration < 0) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.piece_duration_nonnegative", "Piece duration must be finite and nonnegative")));
            }

            SynthesisScore result;
            double end = 0;
            const auto appendRest = [&](double start, double duration, const std::string &language) {
                Common::InputWordInfo word;
                word.notes.push_back({0, 0, duration, Common::GlideType::None, true});
                word.phones.push_back({"SP", language, 0, 0, {}});
                result.words.push_back(std::move(word));
                result.origins.push_back({start, {std::nullopt}});
            };

            for (std::size_t i = 0; i < piece.notes.size(); ++i) {
                const auto &note = piece.notes[i];
                const auto noteIndex = std::to_string(i);
                if (!std::isfinite(note.position.gap) || note.position.gap < 0 || !std::isfinite(note.position.duration) || note.position.duration < 0 || note.cent > 12800) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_position_or_pitch_invalid", "Note {0} requires finite nonnegative gap/duration and cent in [0,12800]", noteIndex)));
                }

                const double start = end + note.position.gap;
                const double nextEnd = start + note.position.duration;
                if (!std::isfinite(nextEnd) || nextEnd > piece.pieceDuration + 1e-9 * std::max(1.0, piece.pieceDuration)) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_exceeds_piece_duration", "Note {0} ends beyond piece duration {1}", noteIndex, std::to_string(piece.pieceDuration))));
                }

                result.noteStarts.push_back(start);
                result.phonemeCounts.push_back(note.phonemes.size());
                const bool rest = reserved(singer, note.pronunciation);
                Common::InputNoteInfo nativeNote{static_cast<int>(note.cent / 100), static_cast<int>(note.cent % 100), note.position.duration, Common::GlideType::None, rest};
                if (note.kind == arch::NoteKind::Slur) {
                    if (!i || note.position.gap != 0 || !note.phonemes.empty() || reserved(singer, piece.notes[i - 1].pronunciation) || result.words.empty() || result.words.back().notes.back().is_rest) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_slur_invalid", "Note {0} is a slur without a contiguous preceding singing note, or introduces phonemes", noteIndex)));
                    }
                    nativeNote.is_rest = false;
                    result.words.back().notes.push_back(nativeNote);
                    end = nextEnd;
                    continue;
                }
                if (note.kind != arch::NoteKind::Normal) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_kind_unknown", "Note {0} has an unknown note kind", noteIndex)));
                }

                const auto language = note.language.empty() ? singer.info.defaultLanguage : note.language;
                if (note.position.gap > 0) {
                    appendRest(end, note.position.gap, language);
                }

                if (!rest && note.phonemes.empty()) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_phonemes_missing", "Note {0} has no phonemes", noteIndex)));
                }

                const auto onset = std::ranges::find_if(note.phonemes, [](const auto &phone) { return phone.onset; });
                if (!rest && onset == note.phonemes.end()) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_onset_missing", "Note {0} has no singing phoneme group marked with onset", noteIndex)));
                }

                const std::size_t header = rest ? 0 : static_cast<std::size_t>(onset - note.phonemes.begin());
                if (header && result.words.empty()) {
                    // A zero-length head word carries anticipation without adding time.
                    appendRest(0, 0, language);
                }

                Common::InputWordInfo word;
                word.notes.push_back(nativeNote);
                WordOrigins origins{start, {}};
                for (std::size_t j = 0; j < note.phonemes.size(); ++j) {
                    const auto &phone = note.phonemes[j];
                    if (phone.token.empty() || !std::isfinite(startOf(phone))) {
                        return fail<arch::SynthesisErrors>(arch::InvalidInputError(locale.text("diffsinger.note_phoneme_invalid", "Note {0} phoneme {1} has an empty token or nonfinite start", noteIndex, std::to_string(j))));
                    }
                    Common::InputPhonemeInfo nativePhone{phone.token, phone.language.empty() ? language : phone.language, 0, startOf(phone), {}};
                    if (j < header) {
                        nativePhone.start += start - result.origins.back().start;
                        result.words.back().phones.push_back(std::move(nativePhone));
                        result.origins.back().phonemes.push_back(PhonemeOrigin{i, j});
                    } else {
                        word.phones.push_back(std::move(nativePhone));
                        origins.phonemes.push_back(PhonemeOrigin{i, j});
                    }
                }

                if (rest && word.phones.empty()) {
                    word.phones.push_back({note.pronunciation, language, 0, 0, {}});
                    origins.phonemes.push_back(std::nullopt);
                }
                // Reserved does not imply silence: AP can generate an audible
                // breath, and package-specific tokens have their own semantics.
                const bool silence = rest && note.pronunciation == "SP" && std::ranges::all_of(note.phonemes, [](const auto &phone) { return phone.token == "SP"; });
                result.silent = result.silent && silence;
                result.words.push_back(std::move(word));
                result.origins.push_back(std::move(origins));
                end = nextEnd;
            }

            if (end < piece.pieceDuration) {
                appendRest(end, piece.pieceDuration - end, singer.info.defaultLanguage);
            }

            return result;
        }

        std::vector<double> weightsAt(std::span<const std::vector<double>> mix, double rate, std::size_t sources, double time) {
            if (sources == 1) {
                return {1};
            }

            const double position = std::clamp(time * rate, 0.0, static_cast<double>(mix.size() - 1));
            const auto left = static_cast<std::size_t>(position);
            const auto right = std::min(left + 1, mix.size() - 1);
            std::vector<double> result(sources);
            double sum = 0;
            for (std::size_t i = 0; i + 1 < sources; ++i) {
                result[i] = std::lerp(mix[left][i], mix[right][i], position - static_cast<double>(left));
                sum += result[i];
            }

            result.back() = 1 - sum;
            return result;
        }
    }

    arch::SynthesisResult<SynthesisScore> synthesisScore(const SingerRecord &singer, const arch::Piece<arch::DurationNote> &piece, const localization::Context &locale) { return buildScore(singer, piece, locale); }

    arch::SynthesisResult<SynthesisScore> synthesisScore(const SingerRecord &singer, const arch::Piece<arch::ParameterNote> &piece, const localization::Context &locale) { return buildScore(singer, piece, locale); }

    arch::SynthesisResult<void> validateMix(const SynthesisContext &context, std::span<const std::vector<double>> mix, double rate) {
        if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(1 / rate) || context.sources.empty() || (context.sources.size() > 1 && mix.empty())) {
            return fail<arch::SynthesisErrors>(arch::InvalidInputError(context.locale.text("diffsinger.speaker_mix_rate_or_samples_invalid", "Speaker mix requires a finite positive rate and samples for multiple sources")));
        }

        for (std::size_t i = 0; i < mix.size(); ++i) {
            const auto &row = mix[i];
            const double sum = std::accumulate(row.begin(), row.end(), 0.0);
            if (row.size() + 1 != context.sources.size() || !std::isfinite(sum) || sum > 1 || std::ranges::any_of(row, [](double value) { return !std::isfinite(value) || value < 0 || value > 1; })) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(context.locale.text("diffsinger.speaker_mix_row_invalid", "Speaker mix row {0} must contain N-1 finite weights in [0,1] with sum at most 1", std::to_string(i))));
            }
        }

        return {};
    }

    arch::SynthesisResult<StageScore> stageScore(const SynthesisScore &score, const SynthesisContext &context, std::string_view stage, std::span<const std::vector<double>> mix, double rate) {
        auto valid = validateMix(context, mix, rate);
        if (!valid) {
            return std::unexpected{std::move(valid.error())};
        }

        StageScore result{score.words, {}};
        std::vector<std::string> targets;
        for (const auto &source : context.sources) {
            const auto found = source.singer->stages.find(std::string(stage));
            if (found == source.singer->stages.end() || !found->second.spec) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(context.locale.text("diffsinger.singer_stage_unavailable", "Singer {0} does not provide stage {1}", source.singer->info.id, stage)));
            }
            const auto &record = found->second;
            if (!record.usesSpeakers) {
                return result;
            }

            auto target = source.speaker;
            if (!record.speakerMapping.empty()) {
                const auto mapping = record.speakerMapping.find(target);
                if (mapping == record.speakerMapping.end()) {
                    return fail<arch::SynthesisErrors>(arch::InvalidInputError(context.locale.text("diffsinger.singer_stage_speaker_unmapped", "Singer {0} stage {1} cannot map logical speaker {2}", source.singer->info.id, stage, target)));
                }
                target = mapping->second;
            }
            if (!record.embeddedSpeakers.contains(target) || std::ranges::find(record.speakers, target) == record.speakers.end()) {
                return fail<arch::SynthesisErrors>(arch::InvalidInputError(context.locale.text("diffsinger.singer_stage_embedding_missing", "Singer {0} stage {1} has no valid embedding for {2}", source.singer->info.id, stage, target)));
            }
            targets.push_back(std::move(target));
        }

        const auto mappedWeights = [&](double time) {
            const auto weights = weightsAt(mix, rate, targets.size(), time);
            std::map<std::string, double> mapped;
            for (std::size_t i = 0; i < weights.size(); ++i) {
                mapped[targets[i]] += weights[i];
            }
            return mapped;
        };

        for (std::size_t i = 0; i < result.words.size(); ++i) {
            for (auto &phone : result.words[i].phones) {
                for (const auto &[speaker, weight] : mappedWeights(score.origins[i].start + phone.start)) {
                    phone.speakers.push_back({speaker, weight});
                }
            }
        }

        const auto rows = std::max<std::size_t>(mix.size(), 1);
        std::map<std::string, std::vector<double>> curves;
        for (std::size_t i = 0; i < rows; ++i) {
            for (const auto &[speaker, weight] : mappedWeights(static_cast<double>(i) / rate)) {
                curves[speaker].push_back(weight);
            }
        }

        for (auto &[speaker, values] : curves) {
            result.speakers.push_back({std::move(speaker), 1 / rate, std::move(values)});
        }

        return result;
    }

    arch::SynthesisResult<arch::DurationOutput> durationOutput(const SynthesisScore &score, std::span<const double> durations) {
        std::size_t count = 0;
        for (const auto &word : score.words) {
            count += word.phones.size();
        }
        if (durations.size() != count || std::ranges::any_of(durations, [](double value) { return !std::isfinite(value) || value < 0; })) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Duration inference returned invalid durations or a phoneme count different from " + std::to_string(count)));
        }

        arch::DurationOutput result;
        for (const auto count : score.phonemeCounts) {
            result.notes.push_back({std::vector<arch::DurationOutputPhoneme>(count)});
        }

        std::size_t index = 0;
        for (std::size_t i = 0; i < score.words.size(); ++i) {
            double start = score.origins[i].start;
            for (const auto &origin : score.origins[i].phonemes) {
                if (origin) {
                    result.notes[origin->note].phonemes[origin->phoneme].start = start - score.noteStarts[origin->note];
                }
                start += durations[index++];
                if (!std::isfinite(start)) {
                    return fail<arch::SynthesisErrors>(arch::OperationError("Duration inference overflowed the phoneme time axis"));
                }
            }
        }

        return result;
    }
}
