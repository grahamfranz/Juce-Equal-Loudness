#include "juce_equal_loudness.h"

#include <cmath>

namespace juce_equal_loudness
{

//==============================================================================
// ISO226::splAtFrequency
//==============================================================================
namespace
{
    // Linear interpolation of a per-frequency coefficient at an arbitrary
    // frequency, in log-frequency space. Outside the tabulated range the
    // nearest endpoint coefficient is used (the caller is expected to clamp
    // the frequency to a sensible audible range first).
    float interpolateCoefficient (const std::array<float, ISO226::numFrequencies>& values,
                                  float frequency) noexcept
    {
        const auto& freqs = ISO226::frequencies;

        if (frequency <= freqs.front()) return values.front();
        if (frequency >= freqs.back())  return values.back();

        // Find the bracketing pair. 29 entries is small enough that a linear
        // scan is faster than std::lower_bound for cache/branch reasons.
        int hi = 1;
        while (hi < ISO226::numFrequencies && freqs[(size_t) hi] < frequency)
            ++hi;

        const int lo = hi - 1;
        const float logF  = std::log (frequency);
        const float logLo = std::log (freqs[(size_t) lo]);
        const float logHi = std::log (freqs[(size_t) hi]);
        const float t = (logF - logLo) / (logHi - logLo);

        return values[(size_t) lo] + t * (values[(size_t) hi] - values[(size_t) lo]);
    }
}

float ISO226::splAtFrequency (float frequency, float phon) noexcept
{
    const float f = juce::jlimit (frequencies.front(), frequencies.back(), frequency);

    const float a  = interpolateCoefficient (alpha, f);
    const float lu = interpolateCoefficient (Lu,    f);
    const float tf = interpolateCoefficient (Tf,    f);

    const float Af = 4.47e-3f * (std::pow (10.0f, 0.025f * phon) - 1.15f)
                   + std::pow (0.4f * std::pow (10.0f, (tf + lu) * 0.1f - 9.0f), a);

    return (10.0f / a) * std::log10 (Af) - lu + 94.0f;
}

//==============================================================================
// Filter design
//==============================================================================
namespace
{
    // Design frequencies for the 8-band peaking-filter cascade. Roughly
    // log-spaced across the audible range, deliberately skipping 1 kHz (gain
    // there is always 0 by construction, so a biquad slot would be wasted).
    constexpr std::array<float, EqualLoudnessProcessor::numBiquads> designFrequencies = {{
        50.0f, 125.0f, 250.0f, 500.0f, 2000.0f, 4000.0f, 6300.0f, 10000.0f
    }};

    constexpr float designQ = 1.0f;

    EqualLoudnessProcessor::BiquadCoeffs designPeakingFilter (float centreHz,
                                                              float gainDb,
                                                              float Q,
                                                              double sampleRate) noexcept
    {
        const float w0 = juce::MathConstants<float>::twoPi
                       * centreHz / (float) sampleRate;
        const float cosW0 = std::cos (w0);
        const float alpha = std::sin (w0) / (2.0f * Q);
        const float A     = std::pow (10.0f, gainDb / 40.0f);

        const float b0 = 1.0f + alpha * A;
        const float b1 = -2.0f * cosW0;
        const float b2 = 1.0f - alpha * A;
        const float a0 = 1.0f + alpha / A;
        const float a1 = -2.0f * cosW0;
        const float a2 = 1.0f - alpha / A;

        return {
            b0 / a0,
            b1 / a0,
            b2 / a0,
            a1 / a0,
            a2 / a0
        };
    }

    // Squared magnitude response of one biquad at angular frequency w.
    //
    // We compute |N(e^jw)|^2 and |D(e^jw)|^2 directly as (real)^2 + (imag)^2
    // rather than via the algebraically simpler expansion in cos(w) / cos(2w):
    // at very low w (e.g. a 50 Hz design freq at 48 kHz, w ≈ 0.0065 rad) the
    // expanded form sums three near-cancelling O(1) terms and loses almost
    // all precision, producing a small negative denominator and a NaN gain.
    // The (real)^2 + (imag)^2 form keeps the contributions individually small
    // and is well-conditioned everywhere in the audio band.
    double biquadMagSq (const EqualLoudnessProcessor::BiquadCoeffs& c, double w) noexcept
    {
        const double cosW  = std::cos (w);
        const double sinW  = std::sin (w);
        const double cos2W = std::cos (2.0 * w);
        const double sin2W = std::sin (2.0 * w);

        const double nRe = c.b0 + c.b1 * cosW + c.b2 * cos2W;
        const double nIm =       -c.b1 * sinW - c.b2 * sin2W;
        const double dRe = 1.0  + c.a1 * cosW + c.a2 * cos2W;
        const double dIm =       -c.a1 * sinW - c.a2 * sin2W;

        return (nRe * nRe + nIm * nIm) / (dRe * dRe + dIm * dIm);
    }

    // Combined magnitude response of the full cascade at frequency f, in dB.
    float cascadeGainDb (const std::array<EqualLoudnessProcessor::BiquadCoeffs,
                                          EqualLoudnessProcessor::numBiquads>& coeffs,
                         float f,
                         double sampleRate) noexcept
    {
        const double w = juce::MathConstants<double>::twoPi * f / sampleRate;
        double magSq = 1.0;
        for (const auto& c : coeffs)
            magSq *= biquadMagSq (c, w);
        return (float) (10.0 * std::log10 (magSq));
    }

    // Iterative residual correction. Given a phon level and sample rate,
    // return the 8 converged per-band gains (dB) that the cascade should be
    // designed with so the realised response sits within ~0.05 dB of the
    // analytic ISO 226 target at each design frequency.
    //
    // A naive independent-per-band fit overshoots between design frequencies
    // because each peaking filter's skirts contribute gain at its neighbours'
    // centre frequencies, and that crosstalk adds up. We compensate by
    // repeatedly measuring the realised cascade response at each design
    // frequency, subtracting from the target, and folding the residual back
    // into the filter gain.
    std::array<float, EqualLoudnessProcessor::numBiquads>
    solveCascadeGains (float phon, double sampleRate) noexcept
    {
        constexpr int numBiquads = EqualLoudnessProcessor::numBiquads;

        std::array<float, numBiquads> targetDb {};
        for (int i = 0; i < numBiquads; ++i)
            targetDb[(size_t) i] = ISO226::splAtFrequency (designFrequencies[(size_t) i], phon) - phon;

        std::array<EqualLoudnessProcessor::BiquadCoeffs, numBiquads> coeffs {};
        std::array<float, numBiquads> filterGainDb = targetDb;

        auto redesign = [&]
        {
            for (int i = 0; i < numBiquads; ++i)
                coeffs[(size_t) i] = designPeakingFilter (designFrequencies[(size_t) i],
                                                          filterGainDb[(size_t) i],
                                                          designQ,
                                                          sampleRate);
        };

        redesign();

        // Damping factor < 1 trades convergence speed for stability. At full
        // strength (1.0) the iteration overshoots when target gains are large
        // (e.g. +30 dB at 50 Hz, low phon levels): the corrected coefficients
        // produce deep notches elsewhere in the cascade, the realised response
        // collapses, and the next residual blows up. 0.5 converges in ~15
        // iterations and stays well-behaved over the full ISO 226 phon range.
        constexpr int   maxIterations = 20;
        constexpr float damping       = 0.5f;
        constexpr float convergenceDb = 0.05f;

        for (int iter = 0; iter < maxIterations; ++iter)
        {
            float maxResidual = 0.0f;
            for (int i = 0; i < numBiquads; ++i)
            {
                const float realisedDb = cascadeGainDb (coeffs,
                                                        designFrequencies[(size_t) i],
                                                        sampleRate);
                const float residual = targetDb[(size_t) i] - realisedDb;
                filterGainDb[(size_t) i] += damping * residual;
                maxResidual = juce::jmax (maxResidual, std::abs (residual));
            }
            redesign();
            if (maxResidual < convergenceDb) break;
        }

        return filterGainDb;
    }
}

//==============================================================================
// EqualLoudnessProcessor
//==============================================================================
EqualLoudnessProcessor::EqualLoudnessProcessor() = default;

void EqualLoudnessProcessor::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    state_.assign ((size_t) spec.numChannels, {});

    // Precompute the converged per-band gains for every integer phon level
    // in the ISO 226 validated range. The iterative residual-correction loop
    // is sample-rate dependent (it measures the realised cascade response),
    // so the LUT is rebuilt whenever prepare() is called. ~71 entries × 8
    // floats ≈ 2 KB; iteration cost is bounded and one-shot.
    for (int i = 0; i < numLutEntries; ++i)
    {
        const float phon = ISO226::minPhon + (float) i;
        gainLut_[(size_t) i] = solveCascadeGains (phon, sampleRate_);
    }

    // Smoother is advanced once per smoothingChunkSize samples inside
    // process(), so its "sample rate" is the chunk rate, not the audio rate.
    // 0.05 s ramp is short enough to feel responsive and long enough to be
    // inaudible for the size of coefficient deltas the LUT lerp produces.
    phonSmoother_.reset (sampleRate_ / (double) smoothingChunkSize, 0.05);
    phonSmoother_.setCurrentAndTargetValue (phonLevel_);

    updateCoefficientsFromGains (gainsForPhon (phonLevel_));
    reset();
}

void EqualLoudnessProcessor::reset() noexcept
{
    for (auto& channel : state_)
        for (auto& bq : channel)
            bq = {};
}

void EqualLoudnessProcessor::setPhonLevel (float phon) noexcept
{
    phonLevel_ = juce::jlimit (ISO226::minPhon, ISO226::maxPhon, phon);
    phonSmoother_.setTargetValue (phonLevel_);
}

std::array<float, EqualLoudnessProcessor::numBiquads>
EqualLoudnessProcessor::gainsForPhon (float phon) const noexcept
{
    const float clamped = juce::jlimit (ISO226::minPhon, ISO226::maxPhon, phon);
    const float pos     = clamped - ISO226::minPhon;
    const int   lo      = juce::jlimit (0, numLutEntries - 1, (int) pos);
    const int   hi      = juce::jmin (lo + 1, numLutEntries - 1);
    const float t       = pos - (float) lo;

    std::array<float, numBiquads> gains {};
    for (int i = 0; i < numBiquads; ++i)
    {
        const float a = gainLut_[(size_t) lo][(size_t) i];
        const float b = gainLut_[(size_t) hi][(size_t) i];
        gains[(size_t) i] = a + t * (b - a);
    }
    return gains;
}

void EqualLoudnessProcessor::updateCoefficientsFromGains (
    const std::array<float, numBiquads>& gainsDb) noexcept
{
    for (int i = 0; i < numBiquads; ++i)
        coeffs_[(size_t) i] = designPeakingFilter (designFrequencies[(size_t) i],
                                                   gainsDb[(size_t) i],
                                                   designQ,
                                                   sampleRate_);
}

float EqualLoudnessProcessor::gainAtFrequency (float frequency) const noexcept
{
    return ISO226::splAtFrequency (frequency, phonLevel_) - phonLevel_;
}

void EqualLoudnessProcessor::process (juce::AudioBuffer<float>& buffer) noexcept
{
    process (juce::dsp::AudioBlock<float> (buffer));
}

void EqualLoudnessProcessor::process (juce::dsp::AudioBlock<float> block) noexcept
{
    const auto numChannels = (size_t) juce::jmin ((int) block.getNumChannels(),
                                                  (int) state_.size());
    const auto numSamples  = block.getNumSamples();

    // Split the block into small sub-blocks; at each boundary advance the
    // phon smoother by one "tick" (= smoothingChunkSize audio samples worth)
    // and rebuild the cascade from the LUT. While the smoother is not
    // ramping we skip the redesign entirely.
    size_t pos = 0;
    while (pos < numSamples)
    {
        const size_t chunk = juce::jmin (numSamples - pos, (size_t) smoothingChunkSize);

        if (phonSmoother_.isSmoothing())
        {
            const float smoothedPhon = phonSmoother_.getNextValue();
            updateCoefficientsFromGains (gainsForPhon (smoothedPhon));
        }

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* samples = block.getChannelPointer (ch) + pos;
            auto& channelState = state_[ch];

            for (size_t n = 0; n < chunk; ++n)
            {
                float x = samples[n];

                for (int i = 0; i < numBiquads; ++i)
                {
                    const auto& c = coeffs_[(size_t) i];
                    auto& s = channelState[(size_t) i];

                    // Direct Form I: state is past signal samples, not
                    // coefficient-scaled intermediates, so coefficient updates
                    // between samples do not perturb the state's meaning. See
                    // the BiquadState comment in the header.
                    const float y = c.b0 * x  + c.b1 * s.x1 + c.b2 * s.x2
                                              - c.a1 * s.y1 - c.a2 * s.y2;
                    s.x2 = s.x1; s.x1 = x;
                    s.y2 = s.y1; s.y1 = y;
                    x = y;
                }

                samples[n] = x;
            }
        }

        pos += chunk;
    }
}

} // namespace juce_equal_loudness
