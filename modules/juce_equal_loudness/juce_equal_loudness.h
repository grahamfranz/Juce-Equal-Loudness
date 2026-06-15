/*
  ==============================================================================

   BEGIN_JUCE_MODULE_DECLARATION

    ID:                 juce_equal_loudness
    vendor:             Graham Franz
    version:            0.2.0
    name:               JUCE Equal Loudness
    description:        Real-time ISO 226:2003 equal-loudness compensation via an IIR peaking-filter cascade.
    website:            https://github.com/grahamfranz/Juce-Equal-Loudness
    license:            MIT
    minimumCppStandard: 17
    dependencies:       juce_core, juce_dsp

   END_JUCE_MODULE_DECLARATION

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace juce_equal_loudness
{

//==============================================================================
// ISO 226:2003 reference data.
//
// The 29 reference frequencies and their per-frequency coefficients
// (alpha_f, L_U, T_f) from Table 1 of ISO 226:2003. These define the
// standard's closed-form relationship between sound pressure level Lp and
// loudness level L_N at each frequency:
//
//     A_f = 4.47e-3 * (10^(0.025 * L_N) - 1.15)
//         + (0.4 * 10^((T_f + L_U)/10 - 9))^alpha_f
//     L_p = (10 / alpha_f) * log10(A_f) - L_U + 94
//
// At 1 kHz, alpha_f = 0.25, L_U = 0, T_f = 2.4, and the formula reduces to
// L_p == L_N (the definition of the phon).
//==============================================================================
struct ISO226
{
    static constexpr int numFrequencies = 29;

    static constexpr std::array<float, numFrequencies> frequencies = {{
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f,
        200.0f, 250.0f, 315.0f, 400.0f, 500.0f, 630.0f, 800.0f, 1000.0f, 1250.0f,
        1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f,
        10000.0f, 12500.0f
    }};

    static constexpr std::array<float, numFrequencies> alpha = {{
        0.532f, 0.506f, 0.480f, 0.455f, 0.432f, 0.409f, 0.387f, 0.367f, 0.349f, 0.330f,
        0.315f, 0.301f, 0.288f, 0.276f, 0.267f, 0.259f, 0.253f, 0.250f, 0.246f,
        0.244f, 0.243f, 0.243f, 0.243f, 0.242f, 0.242f, 0.245f, 0.254f, 0.271f, 0.301f
    }};

    static constexpr std::array<float, numFrequencies> Lu = {{
        -31.6f, -27.2f, -23.0f, -19.1f, -15.9f, -13.0f, -10.3f, -8.1f, -6.2f, -4.5f,
         -3.1f,  -2.0f,  -1.1f,  -0.4f,   0.0f,   0.3f,   0.5f,  0.0f, -2.7f,
         -4.1f,  -1.0f,   1.7f,   2.5f,   1.2f,  -2.1f,  -7.1f, -11.2f, -10.7f, -3.1f
    }};

    static constexpr std::array<float, numFrequencies> Tf = {{
        78.5f, 68.7f, 59.5f, 51.1f, 44.0f, 37.5f, 31.5f, 26.5f, 22.1f, 17.9f,
        14.4f, 11.4f,  8.6f,  6.2f,  4.4f,  3.0f,  2.2f,  2.4f,  3.5f,
         1.7f, -1.3f, -4.2f, -6.0f, -5.4f, -1.5f,  6.0f, 12.6f, 13.9f, 12.3f
    }};

    // The lowest and highest phon levels for which ISO 226:2003 considers the
    // formula validated. The closed form is mathematically well defined outside
    // this range, but the standard does not claim accuracy there.
    static constexpr float minPhon = 20.0f;
    static constexpr float maxPhon = 90.0f;

    // SPL (dB) on the equal-loudness contour at the given frequency and phon
    // level. Frequencies outside [20, 12500] Hz are clamped. For frequencies
    // between the 29 reference points, the per-frequency coefficients are
    // linearly interpolated in log-frequency.
    static float splAtFrequency (float frequency, float phon) noexcept;
};

//==============================================================================
// EqualLoudnessProcessor
//
// Real-time loudness compensation. Designs an 8-band peaking-filter cascade
// whose magnitude response approximates the inverse of the ISO 226:2003 equal-
// loudness contour at the chosen phon level, normalised so the response is
// 0 dB at 1 kHz. Flat input audio processed through this filter will sound
// approximately spectrally balanced to a listener whose ear is operating at
// that phon level.
//
// Real-time safety:
//   - prepare() is the only function that allocates. It also precomputes a
//     lookup table of converged per-band gains across the ISO 226 phon range,
//     so the iterative biquad-design loop never runs on the audio thread.
//   - process(), reset(), and setPhonLevel() do not allocate.
//   - The filter cascade has a fixed size (numBiquads). Changing the phon
//     level updates coefficients in place; it does not resize.
//   - setPhonLevel() is cheap: it stores the target and lets the internal
//     linear smoother ramp toward it across subsequent process() calls.
//
// Parameter smoothing:
//   - Phon changes are ramped over ~50 ms by a linear smoother. Inside
//     process(), the block is split into small sub-blocks; at each sub-block
//     boundary the smoothed phon is looked up in the gain LUT and the 8
//     biquads are redesigned from the interpolated gains. This avoids the
//     audible click that a step change in coefficients would produce.
//
// Limitations:
//   - Single phon parameter, no separate playback/reference contour.
//   - float samples only.
//==============================================================================
class EqualLoudnessProcessor
{
public:
    static constexpr int numBiquads = 8;

    EqualLoudnessProcessor();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset() noexcept;

    void setPhonLevel (float phon) noexcept;
    float getPhonLevel() const noexcept { return phonLevel_; }

    void process (juce::AudioBuffer<float>& buffer) noexcept;
    void process (juce::dsp::AudioBlock<float> block) noexcept;

    // Modeled magnitude response at the given frequency, in dB. Includes the
    // 1 kHz normalisation, so this returns 0 at 1 kHz for any phon level.
    // Computed analytically from the ISO 226 contour (not from the actual
    // designed biquads), so it is an exact reference, not a measurement.
    float gainAtFrequency (float frequency) const noexcept;

    struct BiquadCoeffs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    // Direct Form I state: two past inputs and two past outputs. DF-I is used
    // (rather than the more memory-efficient DF-II) because its state is
    // coefficient-independent — past signal samples, not coefficient-scaled
    // intermediates. That makes it the standard choice for time-varying
    // biquads: as setPhonLevel() ramps and updateCoefficientsFromGains()
    // rewrites coefficients per sub-block, DF-I avoids the subtle "zipper"
    // transients DF-II produces when its z-state is reinterpreted under new
    // coefficients.
    struct BiquadState  { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

private:
    // Sub-block size used for parameter smoothing inside process(). The block
    // is split into chunks of this many samples; at each chunk boundary the
    // smoothed phon is advanced and the cascade is redesigned from the LUT.
    // ~32 samples ≈ 0.7 ms at 48 kHz — well below the click-audibility
    // threshold with DF-I biquads, and cheap enough to redesign 8 biquads
    // per chunk. Redesign is skipped entirely when the smoother is settled.
    static constexpr int smoothingChunkSize = 32;

    // LUT covers integer phon values across the ISO 226 validated range.
    static constexpr int numLutEntries =
        (int) (ISO226::maxPhon - ISO226::minPhon) + 1;

    std::array<float, numBiquads> gainsForPhon (float phon) const noexcept;
    void updateCoefficientsFromGains (const std::array<float, numBiquads>& gainsDb) noexcept;

    float phonLevel_ = 60.0f;
    double sampleRate_ = 0.0;

    std::array<BiquadCoeffs, numBiquads> coeffs_ {};
    std::vector<std::array<BiquadState, numBiquads>> state_;

    // Each LUT entry holds the converged per-band gain (dB) that the
    // iterative residual-correction loop produces for that phon level. At
    // runtime we just lerp between the two bracketing entries and redesign
    // the 8 biquads from those gains — no iteration on the audio thread.
    std::array<std::array<float, numBiquads>, numLutEntries> gainLut_ {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> phonSmoother_;
};

} // namespace juce_equal_loudness
