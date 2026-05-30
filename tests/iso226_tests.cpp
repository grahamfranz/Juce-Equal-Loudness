// Tests for juce_equal_loudness.
//
// The ISO 226:2003 formula and coefficients are reproduced verbatim in
// juce_equal_loudness.h; these tests assert the closed-form values it must
// produce by definition and at a handful of published reference points.

#include "../modules/juce_equal_loudness/juce_equal_loudness.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace juce_equal_loudness;

//==============================================================================
// ISO226::splAtFrequency
//==============================================================================

TEST (ISO226, OnePhonEqualsSPLAtOneKilohertz)
{
    // By definition of the phon, the equal-loudness contour at 1 kHz crosses
    // SPL equal to the phon level. The published ISO 226:2003 closed form
    // does not satisfy this exactly because the threshold-of-hearing term
    // doesn't vanish at 1 kHz — it produces ~0.01-0.05 dB deviation. That's
    // a property of the standard, well below perceptual thresholds; the
    // tolerance reflects it.
    for (float phon : { 20.0f, 40.0f, 60.0f, 80.0f, 90.0f })
        EXPECT_NEAR (ISO226::splAtFrequency (1000.0f, phon), phon, 0.1f)
            << "at phon = " << phon;
}

TEST (ISO226, ReferenceContourPointsMatchPublishedValues)
{
    // Reference points from the ISO 226:2003 contour table. Tolerances are
    // 0.5 dB which is well within the standard's stated precision and covers
    // the small numerical noise of the closed-form evaluation.
    struct Point { float freq, phon, expectedSPL; };
    const Point points[] = {
        {  100.0f, 40.0f, 64.3f },
        { 1000.0f, 40.0f, 40.0f },
        {  100.0f, 60.0f, 78.5f },
        { 1000.0f, 60.0f, 60.0f },
        { 4000.0f, 60.0f, 57.6f },
        { 1000.0f, 80.0f, 80.0f },
    };

    for (auto p : points)
    {
        const float spl = ISO226::splAtFrequency (p.freq, p.phon);
        EXPECT_NEAR (spl, p.expectedSPL, 0.5f)
            << "at freq = " << p.freq << ", phon = " << p.phon;
    }
}

TEST (ISO226, LowFrequenciesRequireHigherSPLThanOneKilohertz)
{
    // The ear is much less sensitive below ~500 Hz, so equal-loudness contours
    // rise toward the bass.
    for (float phon : { 30.0f, 60.0f, 80.0f })
    {
        EXPECT_GT (ISO226::splAtFrequency (50.0f, phon), phon + 10.0f);
        EXPECT_GT (ISO226::splAtFrequency (100.0f, phon), phon);
    }
}

TEST (ISO226, MostSensitiveRegionIsAroundThreeToFourKilohertz)
{
    // The contour dips below the 1 kHz reference in the 2.5-4 kHz region for
    // all reasonable phon levels.
    for (float phon : { 30.0f, 60.0f, 80.0f })
    {
        EXPECT_LT (ISO226::splAtFrequency (3150.0f, phon), phon);
        EXPECT_LT (ISO226::splAtFrequency (4000.0f, phon), phon);
    }
}

TEST (ISO226, MonotonicInPhonAtFixedFrequency)
{
    for (float freq : { 50.0f, 200.0f, 1000.0f, 4000.0f, 10000.0f })
    {
        float prev = -std::numeric_limits<float>::infinity();
        for (float phon = 20.0f; phon <= 90.0f; phon += 10.0f)
        {
            const float spl = ISO226::splAtFrequency (freq, phon);
            EXPECT_GT (spl, prev) << "at freq = " << freq << ", phon = " << phon;
            prev = spl;
        }
    }
}

TEST (ISO226, OutOfRangeFrequenciesClampAndReturnFinite)
{
    for (float freq : { 5.0f, 20000.0f, -1.0f, 1e9f })
    {
        const float spl = ISO226::splAtFrequency (freq, 60.0f);
        EXPECT_TRUE (std::isfinite (spl)) << "at freq = " << freq;
    }
}

//==============================================================================
// EqualLoudnessProcessor
//==============================================================================

class ProcessorTest : public ::testing::Test
{
protected:
    EqualLoudnessProcessor proc;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };

    void SetUp() override
    {
        proc.prepare (spec);
    }
};

TEST_F (ProcessorTest, DefaultPhonLevelIsSixty)
{
    EXPECT_FLOAT_EQ (proc.getPhonLevel(), 60.0f);
}

TEST_F (ProcessorTest, SetPhonLevelClampsToValidISORange)
{
    proc.setPhonLevel (10.0f);
    EXPECT_FLOAT_EQ (proc.getPhonLevel(), ISO226::minPhon);

    proc.setPhonLevel (150.0f);
    EXPECT_FLOAT_EQ (proc.getPhonLevel(), ISO226::maxPhon);

    proc.setPhonLevel (70.0f);
    EXPECT_FLOAT_EQ (proc.getPhonLevel(), 70.0f);
}

TEST_F (ProcessorTest, GainAtOneKilohertzIsApproximatelyZero)
{
    // Same caveat as ISO226.OnePhonEqualsSPLAtOneKilohertz: the ISO 226:2003
    // closed form leaves ~0.05 dB of residue at the 1 kHz reference.
    for (float phon : { 30.0f, 60.0f, 90.0f })
    {
        proc.setPhonLevel (phon);
        EXPECT_NEAR (proc.gainAtFrequency (1000.0f), 0.0f, 0.1f);
    }
}

TEST_F (ProcessorTest, GainIsPositiveAtLowFrequencyForLowPhon)
{
    proc.setPhonLevel (40.0f);
    EXPECT_GT (proc.gainAtFrequency (50.0f),  10.0f);
    EXPECT_GT (proc.gainAtFrequency (100.0f),  5.0f);
}

TEST_F (ProcessorTest, ProcessZerosProducesZeros)
{
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    proc.process (buffer);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            EXPECT_FLOAT_EQ (buffer.getSample (ch, i), 0.0f);
}

TEST_F (ProcessorTest, ProcessSineProducesFiniteOutput)
{
    juce::AudioBuffer<float> buffer (2, 2048);
    const float twoPi = juce::MathConstants<float>::twoPi;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float s = std::sin (twoPi * 440.0f * (float) i / 48000.0f);
        buffer.setSample (0, i, s);
        buffer.setSample (1, i, s);
    }

    proc.process (buffer);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            ASSERT_TRUE (std::isfinite (buffer.getSample (ch, i)));
}

TEST_F (ProcessorTest, ChangingPhonLevelDoesNotCrashDuringProcessing)
{
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    for (float phon : { 30.0f, 45.0f, 60.0f, 75.0f, 90.0f })
    {
        proc.setPhonLevel (phon);
        proc.process (buffer);
    }
}

TEST_F (ProcessorTest, ResetClearsFilterState)
{
    juce::AudioBuffer<float> buffer (2, 512);
    for (int i = 0; i < 512; ++i)
    {
        buffer.setSample (0, i, 1.0f);
        buffer.setSample (1, i, 1.0f);
    }

    proc.process (buffer);
    proc.reset();

    // After reset, a buffer of zeros must produce zeros (no residual state).
    buffer.clear();
    proc.process (buffer);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            EXPECT_FLOAT_EQ (buffer.getSample (ch, i), 0.0f);
}
