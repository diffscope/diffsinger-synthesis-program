#include "LanguageResults.h"

#include <algorithm>
#include <cstdint>

#include <stdcorelib/str.h>

namespace dssp::arch_diffsinger {
    namespace {
        using G2PError = wolf::Api::G2p::L1::Error;
        std::string_view errorCode(G2PError error) {
            switch (error) {
                case G2PError::None: return "None";
                case G2PError::InvalidInput: return "InvalidInput";
                case G2PError::PhonemeGenerationFailed: return "PhonemeGenerationFailed";
                case G2PError::ModelInferenceFailed: return "ModelInferenceFailed";
                case G2PError::DriverUnavailable: return "DriverUnavailable";
                case G2PError::NotInitialized: return "NotInitialized";
                case G2PError::UnknownError: return "UnknownError";
            }
            return "UnrecognizedError";
        }

        std::size_t sliceEndpoint(std::optional<std::int64_t> endpoint, std::size_t count, std::size_t fallback) {
            if (!endpoint) {
                return fallback;
            }
            if (*endpoint >= 0) {
                return std::min(static_cast<std::uint64_t>(*endpoint), static_cast<std::uint64_t>(count));
            }

            // This also handles INT64_MIN without overflowing its negation.
            const auto distance = static_cast<std::uint64_t>(-(*endpoint + 1)) + 1;
            return distance > count ? 0 : count - distance;
        }
    }

    arch::SynthesisResult<arch::Pronunciation> translatePronunciation(const wolf::Api::G2p::L1::WordOutput &word, const std::string &lyric) {
        if (word.error == G2PError::InvalidInput || word.error == G2PError::PhonemeGenerationFailed) {
            return arch::Pronunciation{lyric, {lyric}};
        }

        if (word.error != G2PError::None) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Pronunciation inference failed", std::make_shared<const ExternalError>(std::string(errorCode(word.error)), NativeDiagnostic{"wolf", "G2P word conversion", std::string(errorCode(word.error))})));
        }
        using Mode = wolf::Api::G2p::L1::Mode;
        if (word.mode == Mode::Skip) {
            return arch::Pronunciation{};
        }
        if (word.mode != Mode::Copy && word.mode != Mode::Convert) {
            return fail<arch::SynthesisErrors>(arch::OperationError("Pronunciation inference returned an invalid conversion mode"));
        }

        arch::Pronunciation result{word.pronunciation, {}};
        for (const auto &candidate : word.candidates) {
            const auto trimmed = stdc::str::trim(candidate);
            if (!trimmed.empty()) {
                result.candidates.emplace_back(trimmed);
            }
        }

        // The editor treats dictionary phoneme fragments as one complete
        // pronunciation, rather than offering individual phonemes as alternatives.
        const auto tokens = stdc::str::split(std::string_view(result.pronunciation), " ");
        if (!result.pronunciation.empty() && !result.candidates.empty() && std::ranges::all_of(result.candidates, [&](const auto &candidate) {
                return candidate.find(' ') != std::string::npos ? candidate == result.pronunciation : std::ranges::find(tokens, std::string_view(candidate)) != tokens.end();
            })) {
            result.candidates = {result.pronunciation};
        }
        if (word.mode == Mode::Convert && !result.pronunciation.empty()) {
            if (const auto found = std::ranges::find(result.candidates, result.pronunciation); found != result.candidates.end()) {
                result.candidates.erase(found);
            }
            result.candidates.insert(result.candidates.begin(), result.pronunciation);
        }

        return result;
    }

    std::vector<arch::Phoneme> slicePhonemes(std::vector<arch::Phoneme> phonemes, const arch::PronunciationNote &note) {
        if (phonemes.empty() || (!note.syllableSliceStart && !note.syllableSliceEnd)) {
            return phonemes;
        }

        std::vector<std::size_t> boundaries{0};
        bool foundOnset = false;
        for (std::size_t i = 0; i < phonemes.size(); ++i) {
            if (phonemes[i].onset) {
                if (foundOnset) {
                    boundaries.push_back(i);
                }
                foundOnset = true;
            }
        }

        const auto count = boundaries.size();
        boundaries.push_back(phonemes.size());
        const auto start = sliceEndpoint(note.syllableSliceStart, count, 0);
        const auto end = sliceEndpoint(note.syllableSliceEnd, count, count);
        if (start >= end) {
            return {};
        }

        std::vector<arch::Phoneme> result;
        result.reserve(boundaries[end] - boundaries[start]);
        for (auto i = boundaries[start]; i < boundaries[end]; ++i) {
            result.push_back(std::move(phonemes[i]));
        }

        return result;
    }
}
