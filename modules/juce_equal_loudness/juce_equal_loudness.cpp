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
}

//==============================================================================
// EqualLoudnessProcessor
//==============================================================================
EqualLoudnessProcessor::EqualLoudnessProcessor() = default;

void EqualLoudnessProcessor::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    state_.assign ((size_t) spec.numChannels, {});
    updateFilters();
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
    if (sampleRate_ > 0.0)
        updateFilters();
}

void EqualLoudnessProcessor::updateFilters() noexcept
{
    for (int i = 0; i < numBiquads; ++i)
    {
        const float f = designFrequencies[(size_t) i];
        const float gain = ISO226::splAtFrequency (f, phonLevel_) - phonLevel_;
        coeffs_[(size_t) i] = designPeakingFilter (f, gain, designQ, sampleRate_);
    }
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

    for (size_t ch = 0; ch < numChannels; ++ch)
    {
        auto* samples = block.getChannelPointer (ch);
        auto& channelState = state_[ch];

        for (size_t n = 0; n < numSamples; ++n)
        {
            float x = samples[n];

            for (int i = 0; i < numBiquads; ++i)
            {
                const auto& c = coeffs_[(size_t) i];
                auto& s = channelState[(size_t) i];

                // Direct Form II, transposed-friendly variant.
                const float w = x - c.a1 * s.z1 - c.a2 * s.z2;
                const float y = c.b0 * w + c.b1 * s.z1 + c.b2 * s.z2;
                s.z2 = s.z1;
                s.z1 = w;
                x = y;
            }

            samples[n] = x;
        }
    }
}

} // namespace juce_equal_loudness
