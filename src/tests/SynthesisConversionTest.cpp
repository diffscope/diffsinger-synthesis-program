#include <cmath>
#include <filesystem>
#include <limits>

#include <gtest/gtest.h>

#include <dssparchdiffsinger/SynthesisParameters.h>
#include <dssparchdiffsinger/SynthesisScore.h>
#include <dssplocalization/Catalog.h>
#include <dssplocalization/Context.h>

namespace dssp::tests {
    namespace {
        arch_diffsinger::SingerRecord singer() {
            arch_diffsinger::SingerRecord result;
            result.reservedPhonemes = {"SP", "AP"};
            result.info.defaultLanguage = "eng";
            return result;
        }

        arch::ParameterNote note(double gap, double duration, std::vector<arch::TimedPhoneme> phones) {
            arch::ParameterNote result;
            result.position = {gap, duration};
            result.cent = 6950;
            result.pronunciation = "b a";
            result.language = "eng";
            result.phonemes = std::move(phones);
            return result;
        }
    }

    TEST(SynthesisScore, AnticipationGapsAndDurationOriginsStayOnTheSubmittedTimeline) {
        arch::Piece<arch::ParameterNote> piece;
        piece.pieceDuration = 1;
        piece.notes = {note(0.2, 0.4, {{{"b", false, "eng"}, -0.04}, {{"a", true, "eng"}, 0}}), note(0, 0.3, {{{"b", false, "eng"}, -0.05}, {{"i", true, "eng"}, 0}})};
        const auto score = arch_diffsinger::synthesisScore(singer(), piece);
        ASSERT_TRUE(score);
        ASSERT_EQ(score->words.size(), 4U);
        EXPECT_NEAR(score->words[0].phones[1].start, 0.16, 1e-12);
        EXPECT_NEAR(score->words[1].phones[1].start, 0.35, 1e-12);
        EXPECT_EQ(score->words[1].notes[0].key, 69);
        EXPECT_EQ(score->words[1].notes[0].cents, 50);
        EXPECT_NEAR(score->words.back().notes[0].duration, 0.1, 1e-12);
        const std::vector<double> predicted{0.16, 0.04, 0.35, 0.05, 0.3, 0.1};
        const auto output = arch_diffsinger::durationOutput(*score, predicted);
        ASSERT_TRUE(output);
        ASSERT_EQ(output->notes.size(), 2U);
        ASSERT_EQ(output->notes[0].phonemes.size(), 2U);
        EXPECT_NEAR(output->notes[0].phonemes[0].start, -0.04, 1e-12);
        EXPECT_NEAR(output->notes[0].phonemes[1].start, 0, 1e-12);
        EXPECT_NEAR(output->notes[1].phonemes[0].start, -0.05, 1e-12);
        EXPECT_NEAR(output->notes[1].phonemes[1].start, 0, 1e-12);
        EXPECT_FALSE(arch_diffsinger::durationOutput(*score, std::vector<double>{1}));
    }

    TEST(SynthesisScore, ZeroHeadAndNegativeAnticipationDoNotAddPaddingOrRejectThePiece) {
        arch::Piece<arch::ParameterNote> piece;
        piece.pieceDuration = 0.4;
        piece.notes = {note(0, 0.4, {{{"b", false, "eng"}, -0.04}, {{"a", true, "eng"}, 0}})};
        const auto score = arch_diffsinger::synthesisScore(singer(), piece);
        ASSERT_TRUE(score);
        ASSERT_EQ(score->words.size(), 2U);
        EXPECT_EQ(score->words.front().notes[0].duration, 0);
        EXPECT_EQ(score->words.front().phones[1].start, -0.04);
        EXPECT_EQ(score->noteStarts[0], 0);
    }

    TEST(SynthesisScore, ExplicitSlursExtendThePreviousWordAndCannotCrossRestsOrGaps) {
        arch::Piece<arch::ParameterNote> piece;
        piece.pieceDuration = 1;
        piece.notes = {note(0, 0.5, {{{"a", true, "eng"}, 0}}), note(0, 0.5, {})};
        piece.notes[1].kind = arch::NoteKind::Slur;
        const auto score = arch_diffsinger::synthesisScore(singer(), piece);
        ASSERT_TRUE(score);
        ASSERT_EQ(score->words.size(), 1U);
        EXPECT_EQ(score->words[0].notes.size(), 2U);
        EXPECT_EQ(score->words[0].phones.size(), 1U);
        piece.notes[1].position.gap = 0.1;
        piece.pieceDuration = 1.1;
        EXPECT_FALSE(arch_diffsinger::synthesisScore(singer(), piece));
        piece.notes[1].position.gap = 0;
        piece.notes[0].pronunciation = "SP";
        EXPECT_FALSE(arch_diffsinger::synthesisScore(singer(), piece));
    }

    TEST(SynthesisScore, DistinguishesMissingPhonemesFromMissingOnset) {
        arch::Piece<arch::ParameterNote> piece;
        piece.pieceDuration = 0.5;
        piece.notes = {note(0, 0.5, {})};
        const auto empty = arch_diffsinger::synthesisScore(singer(), piece);
        ASSERT_FALSE(empty);
        EXPECT_EQ(empty.error().asError().message(), "Note 0 has no phonemes");

        const auto errorRoot = std::filesystem::path(DSSP_TEST_RESOURCE_DIR) / "localization" / "errors";
        const localization::Catalog::Source sources[]{{"diffsinger", errorRoot / "diffsinger"}};
        const auto translations = localization::Catalog::load(sources, "en_US");
        ASSERT_TRUE(translations);
        const localization::Context chinese(*translations, "zh-CN");
        const auto localized = arch_diffsinger::synthesisScore(singer(), piece, chinese);
        ASSERT_FALSE(localized);
        EXPECT_EQ(localized.error().asError().message(), "音符 0 没有音素");

        piece.notes[0].phonemes = {{{"a", false, "eng"}, 0}};
        const auto noOnset = arch_diffsinger::synthesisScore(singer(), piece);
        ASSERT_FALSE(noOnset);
        EXPECT_EQ(noOnset.error().asError().message(), "Note 0 has no singing phoneme group marked with onset");
    }

    TEST(SynthesisParameters, NonlinearMappingsRoundTripWithoutIntegerQuantization) {
        for (const auto id : {"pitch", "expressiveness", "energy", "breathiness", "voicing", "tension", "mouth_opening", "gender", "velocity", "tone_shift"}) {
            const auto *definition = arch_diffsinger::parameterDefinition(id);
            ASSERT_NE(definition, nullptr);
            for (const double value : {0.0, 0.137, 0.5, 0.731, 1.0}) {
                EXPECT_NEAR(arch_diffsinger::normalizedValue(*definition, arch_diffsinger::engineValue(*definition, value)), value, 1e-12) << id;
            }
        }

        EXPECT_DOUBLE_EQ(arch_diffsinger::engineValue(*arch_diffsinger::parameterDefinition("pitch"), 0.5390625), 69);
        EXPECT_DOUBLE_EQ(arch_diffsinger::engineValue(*arch_diffsinger::parameterDefinition("velocity"), 0.5), 1);
    }

    TEST(SynthesisParameters, ShortCurvesUsePublicDefaultsAndConditionRetakesAreLocked) {
        const auto energy = arch_diffsinger::parameterCurve("energy", std::vector<double>{0, 0.5}, 4, 1);
        ASSERT_TRUE(energy);
        ASSERT_EQ(energy->values.size(), 5U);
        EXPECT_DOUBLE_EQ(energy->values[0], -96);
        EXPECT_DOUBLE_EQ(energy->values[2], 0);
        ASSERT_TRUE(energy->retake);
        EXPECT_EQ(energy->retake->start, 0);
        EXPECT_EQ(energy->retake->end, 0);
        const auto velocity = arch_diffsinger::parameterCurve("velocity", {}, 4, 1);
        ASSERT_TRUE(velocity);
        EXPECT_EQ(velocity->values, std::vector<double>{1});
        EXPECT_FALSE(arch_diffsinger::parameterCurve("energy", std::vector<double>{1.1}, 4, 1));
        EXPECT_FALSE(arch_diffsinger::parameterCurve("energy", {}, 0, 1));
    }

    TEST(SynthesisParameters, RetakesUseTheInputGridAndRespectCapabilityAndOverflow) {
        arch::IndirectParameter range{{"pitch"}, arch::ParameterRetakeMode::Range};
        const auto retake = arch_diffsinger::parameterRetake({20, 30}, 100, 1, &range);
        ASSERT_TRUE(retake);
        EXPECT_DOUBLE_EQ(retake->start, 0.2);
        EXPECT_DOUBLE_EQ(retake->end, 0.5);
        EXPECT_FALSE(arch_diffsinger::parameterRetake({80, 30}, 100, 1, &range));
        EXPECT_FALSE(arch_diffsinger::parameterRetake({0, 1}, 100, 1, nullptr));
        EXPECT_TRUE(arch_diffsinger::parameterRetake({0, 0}, 100, 1, nullptr));
        EXPECT_FALSE(arch_diffsinger::parameterRetake({std::numeric_limits<std::uint64_t>::max(), 1}, 100, 1, &range));
        range.retakeMode = arch::ParameterRetakeMode::Full;
        EXPECT_FALSE(arch_diffsinger::parameterRetake({20, 30}, 100, 1, &range));
        EXPECT_TRUE(arch_diffsinger::parameterRetake({0, 100}, 100, 1, &range));
    }

    TEST(SynthesisParameters, ToneShiftChangesConditionPitchWhileVocoderKeepsOriginalPitchAndVoicing) {
        const auto pitch = arch_diffsinger::parameterCurve("pitch", std::vector<double>{0, 69.0 / 128, 69.0 / 128}, 100, 0.03);
        const auto tone = arch_diffsinger::parameterCurve("tone_shift", std::vector<double>{1}, 20, 0.03);
        ASSERT_TRUE(pitch);
        ASSERT_TRUE(tone);
        const auto shifted = arch_diffsinger::effectivePitch(*pitch, &*tone, 0.03, 0.01);
        ASSERT_TRUE(shifted);
        EXPECT_EQ(shifted->values, (std::vector<double>{0, 81, 81}));
        EXPECT_EQ(pitch->values[1], 69);
        const auto f0 = arch_diffsinger::vocoderF0(*pitch, 3, 0.01);
        ASSERT_TRUE(f0);
        EXPECT_EQ(*f0, (std::vector<float>{0, 440, 440}));
        const auto fine = arch_diffsinger::vocoderF0(*pitch, 6, 0.005);
        ASSERT_TRUE(fine);
        EXPECT_EQ((*fine)[0], 0);
    }
}
