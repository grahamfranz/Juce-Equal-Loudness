// juce_equal_loudness console demo.
//
// Prints the ISO 226:2003 equal-loudness contour at a chosen phon level, then
// processes a white-noise buffer through the EqualLoudnessProcessor and reports
// per-band RMS before and after, so the boost shape can be inspected without a
// plugin host.
//
// Usage: equal_loudness_demo [phon] [sampleRate] [numSamples]

#include "../modules/juce_equal_loudness/juce_equal_loudness.h"

#include <cstdio>
#include <cstdlib>
#include <random>

using namespace juce_equal_loudness;

namespace
{
    constexpr float displayFrequencies[] = {
        20.0f, 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f,
        2000.0f, 4000.0f, 6300.0f, 10000.0f, 12500.0f
    };

    // Goertzel single-bin magnitude. Used to read out the level at a chosen
    // frequency in a buffer of broadband noise, so we can verify the cascade
    // applies a boost shaped like the ISO 226 contour.
    float goertzelDb (const float* samples, int n, double sampleRate, float freqHz) noexcept
    {
        const double omega = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
        const double coeff = 2.0 * std::cos (omega);

        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            s0 = samples[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const double mag = std::sqrt (std::max (power, 1e-30)) / (double) n;
        return 20.0f * (float) std::log10 (mag + 1e-30);
    }
}

int main (int argc, char** argv)
{
    const float  phon       = argc > 1 ? std::strtof (argv[1], nullptr) : 60.0f;
    const double sampleRate = argc > 2 ? std::strtod (argv[2], nullptr) : 48000.0;
    const int    numSamples = argc > 3 ? std::atoi  (argv[3])           : 32768;

    std::printf ("juce_equal_loudness demo\n");
    std::printf ("  phon       = %.1f\n", phon);
    std::printf ("  sampleRate = %.1f Hz\n", sampleRate);
    std::printf ("  numSamples = %d\n\n", numSamples);

    std::printf ("ISO 226:2003 contour and target compensation gain at %.1f phon:\n", phon);
    std::printf ("  %10s  %10s  %10s\n", "freq (Hz)", "SPL (dB)", "gain (dB)");
    for (float f : displayFrequencies)
    {
        const float spl  = ISO226::splAtFrequency (f, phon);
        const float gain = spl - phon;
        std::printf ("  %10.1f  %10.2f  %+10.2f\n", f, spl, gain);
    }
    std::printf ("\n");

    EqualLoudnessProcessor processor;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) numSamples, 1 };
    processor.prepare (spec);
    processor.setPhonLevel (phon);

    std::vector<float> dry  ((size_t) numSamples);
    std::vector<float> wet  ((size_t) numSamples);
    std::mt19937 rng (1);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    for (int i = 0; i < numSamples; ++i)
        dry[(size_t) i] = wet[(size_t) i] = dist (rng);

    juce::AudioBuffer<float> buffer (1, numSamples);
    std::copy (wet.begin(), wet.end(), buffer.getWritePointer (0));
    processor.process (buffer);

    std::printf ("Measured per-band level on white noise (dry -> wet, delta):\n");
    std::printf ("  %10s  %10s  %10s  %10s\n", "freq (Hz)", "dry (dB)", "wet (dB)", "delta (dB)");
    for (float f : displayFrequencies)
    {
        const float dryDb = goertzelDb (dry.data(), numSamples, sampleRate, f);
        const float wetDb = goertzelDb (buffer.getReadPointer (0), numSamples, sampleRate, f);
        std::printf ("  %10.1f  %10.2f  %10.2f  %+10.2f\n", f, dryDb, wetDb, wetDb - dryDb);
    }

    return 0;
}
