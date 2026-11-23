#include <algorithm>
#include <limits>

#include <gtest/gtest.h>

#include <dsspextraction/Results.h>
#include <dsspextraction/Slicing.h>

namespace dssp::tests {
    namespace F0 = otter::Api::F0::L1;

    TEST(ExtractionSlicing, BalancedHardLimitsPreserveEverySampleIncludingPartialHop) {
        const std::vector<float> samples(10007, 0.5f);
        const auto result = extraction::sliceAudio(samples, 1000, 3, 0, 0);

        ASSERT_TRUE(result);
        ASSERT_EQ(result->size(), 4U);
        EXPECT_EQ(result->front().begin, 0U);
        EXPECT_EQ(result->back().end, samples.size());

        std::size_t previous = 0, minimum = samples.size(), maximum = 0;
        for (const auto &span : *result) {
            EXPECT_EQ(span.begin, previous);
            EXPECT_LE(span.end - span.begin, 3000U);
            minimum = std::min(minimum, span.end - span.begin);
            maximum = std::max(maximum, span.end - span.begin);
            previous = span.end;
        }

        EXPECT_LE(maximum - minimum, 1U);
    }

    TEST(ExtractionSlicing, PitchBoundariesShareTheGlobalGridAndRetainTheTail) {
        const std::vector<float> samples(2001, 0.5f);
        const auto result = extraction::sliceAudio(samples, 16000, 0.04, 0.01, 0);

        ASSERT_TRUE(result);
        ASSERT_GT(result->size(), 1U);

        std::size_t previous = 0;
        for (const auto &span : *result) {
            EXPECT_EQ(span.begin, previous);
            EXPECT_EQ(span.begin % 160, 0U);
            EXPECT_LE(span.end - span.begin, 640U);
            if (span.end != samples.size())
                EXPECT_EQ(span.end % 160, 0U);
            previous = span.end;
        }

        EXPECT_EQ(previous, samples.size());
    }

    TEST(ExtractionSlicing, SilenceGapsDoNotShiftTheRemainingAudio) {
        std::vector<float> samples(9007, 0.5f);
        std::fill(samples.begin() + 3000, samples.begin() + 6000, 0.0f);

        const auto result = extraction::sliceAudio(samples, 1000, 0, 0, 0.02);

        ASSERT_TRUE(result);
        ASSERT_EQ(result->size(), 2U);
        EXPECT_EQ(result->front().begin, 0U);
        EXPECT_LT(result->front().end, result->back().begin);
        EXPECT_EQ(result->back().end, samples.size());

        const auto disabled = extraction::sliceAudio(samples, 1000, 0, 0, 0);
        ASSERT_TRUE(disabled);
        ASSERT_EQ(disabled->size(), 1U);
        EXPECT_EQ(disabled->front().end, samples.size());
    }

    TEST(ExtractionSlicing, SilentInputIsStillAnalyzedAndCancelledWorkStops) {
        const std::vector<float> samples(4000, 0);
        const auto result = extraction::sliceAudio(samples, 1000, 0, 0, 0.02);

        ASSERT_TRUE(result);
        ASSERT_EQ(result->size(), 1U);
        EXPECT_EQ(result->front().end, samples.size());

        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = extraction::sliceAudio(samples, 1000, 0, 0, 0.02, stop.get_token());

        ASSERT_FALSE(cancelled);
        EXPECT_NE(cancelled.error().get_if<extraction::CancelledError>(), nullptr);

        EXPECT_FALSE(extraction::sliceAudio(samples, 1000, 0.001, 0.01, 0));
    }

    TEST(ExtractionResults, AbsoluteNotesAreSortedWithoutAddingTheSpanOffsetAgain) {
        const auto result = extraction::noteOutput({{62, 3, 0.5, 1}, {60, 1, 1, 0.9}}, 4);

        ASSERT_TRUE(result);
        ASSERT_EQ(result->notes.size(), 2U);
        EXPECT_DOUBLE_EQ(result->notes[0].gap, 1);
        EXPECT_DOUBLE_EQ(result->notes[0].duration, 1);
        EXPECT_EQ(result->notes[0].cent, 6000U);
        EXPECT_DOUBLE_EQ(result->notes[1].gap, 1);
        EXPECT_EQ(result->notes[1].cent, 6200U);

        EXPECT_FALSE(extraction::noteOutput({{60, 1, 1, 1}, {62, 1.5, 1, 1}}, 4));
        EXPECT_FALSE(extraction::noteOutput({{129, 0, 1, 1}}, 4));
        EXPECT_FALSE(extraction::noteOutput({{60, std::numeric_limits<double>::quiet_NaN(), 1, 1}}, 4));
    }

    TEST(ExtractionResults, VoicedMaskWinsAndGapCountsFromThePreviousVoicedEnd) {
        extraction::PitchBuilder builder(0.01);
        F0::Result first;
        first.interval = 0.01;
        first.f0 = {440, 440, 440};
        first.voiced = {0, 1, 0};
        ASSERT_TRUE(builder.append(first, 0, 0.03, 16000));
        F0::Result second;
        second.interval = 0.01;
        second.startTime = 0.1;
        second.f0 = {440, 880};
        ASSERT_TRUE(builder.append(second, 0.1, 0.12, 16000));
        const auto result = builder.take();
        EXPECT_DOUBLE_EQ(result.sampleRate, 100);
        ASSERT_EQ(result.segments.size(), 2U);
        EXPECT_EQ(result.segments[0].gap, 1U);
        EXPECT_EQ(result.segments[1].gap, 8U);
        EXPECT_DOUBLE_EQ(result.segments[0].pitch[0], 6900);
        EXPECT_DOUBLE_EQ(result.segments[1].pitch[1], 8100);
    }

    TEST(ExtractionResults, AdjacentSpansMergeAndOnePaddedBoundaryFrameIsDiscarded) {
        extraction::PitchBuilder builder(0.01);
        F0::Result first;
        first.interval = 0.01;
        first.f0 = {440, 440, 440};
        ASSERT_TRUE(builder.append(first, 0, 0.02, 16000));
        F0::Result second;
        second.interval = 0.01;
        second.startTime = 0.02;
        second.f0 = {440, 440};
        ASSERT_TRUE(builder.append(second, 0.02, 0.04, 16000));
        const auto result = builder.take();
        ASSERT_EQ(result.segments.size(), 1U);
        EXPECT_EQ(result.segments.front().pitch.size(), 4U);
    }

    TEST(ExtractionResults, InvalidModelCurvesFailAndUnvoicedInputRetainsFrameRate) {
        F0::Result curve;
        curve.interval = 0.01;
        curve.f0 = {440, 440};
        curve.voiced = {1};

        extraction::PitchBuilder invalidMask(0.01);
        EXPECT_FALSE(invalidMask.append(curve, 0, 0.02, 16000));

        curve.voiced = {0, 0};
        extraction::PitchBuilder silent(0.01);
        ASSERT_TRUE(silent.append(curve, 0, 0.02, 16000));

        const auto result = silent.take();
        EXPECT_TRUE(result.segments.empty());
        EXPECT_DOUBLE_EQ(result.sampleRate, 100);

        curve.voiced = {1, 1};
        curve.f0[0] = std::numeric_limits<float>::infinity();
        extraction::PitchBuilder invalidFrequency(0.01);
        EXPECT_FALSE(invalidFrequency.append(curve, 0, 0.02, 16000));
    }
}
