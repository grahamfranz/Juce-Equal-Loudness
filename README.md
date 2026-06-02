# juce_equal_loudness

[![CI](https://github.com/grahamfranz/Juce-Equal-Loudness/actions/workflows/ci.yml/badge.svg)](https://github.com/grahamfranz/Juce-Equal-Loudness/actions/workflows/ci.yml)

A small JUCE module that applies real-time equal-loudness compensation based
on the ISO 226:2003 standard.

**Status: v0.1.1** See [Scope and limitations](#scope-and-limitations) before
depending on it.

## Why this exists

Human hearing isn't equally sensitive to all frequencies. At quiet listening
levels in particular, the ear's response to bass and high treble falls off
much faster than its response to the midrange — which is why music turned
down sounds thinner and less full, not just quieter. The ISO 226:2003
standard describes this frequency-dependent sensitivity numerically. This
module applies its inverse as a real-time EQ, so that flat-spectrum audio is
shaped to sound spectrally balanced to the ear at a chosen listening level.

Listening level is parameterised in **phon**. The phon is the standard
perceived-loudness unit: a sound is X phon when it sounds as loud to the ear
as an X dB-SPL tone at 1 kHz. Typical levels: conversation around 60 phon,
loud music around 90.

## How it works

Given a phon level `Ln`, the processor builds an 8-band cascade of IIR
peaking filters whose combined magnitude response approximates the inverse
of the ISO 226:2003 equal-loudness contour at `Ln`, normalised to 0 dB at
1 kHz.

## ISO 226 data

The 29 per-frequency coefficients `αf`, `Lu`, `Tf` from ISO 226:2003 Table 1
are stored verbatim in `juce_equal_loudness.h`, and the SPL at any frequency
and phon level is computed from the standard's closed-form expression on
demand. For frequencies between the 29 reference points the coefficients are
linearly interpolated in log-frequency.

The 1 kHz column of the contour equals the phon level (within ~0.05 dB,
which is the formula's intrinsic numerical precision at that point). The
unit tests assert this.

## Building

This module depends on `juce_core` and `juce_dsp`. The included CMake build
expects a JUCE source checkout on disk.

```sh
git clone https://github.com/grahamfranz/Juce-Equal-Loudness.git
cd Juce-Equal-Loudness
cmake -B build -DJUCE_PATH=/path/to/JUCE
cmake --build build
```

`JUCE_PATH` can also be set via an environment variable.

Outputs:

- `build/equal_loudness_demo` — small console program that prints the contour
  and demonstrates the filter on white noise.
- `build/iso226_tests` — GoogleTest binary. Run with `ctest --test-dir build`
  or directly.

## Using the module in your own JUCE project

If you build with the standard JUCE CMake API:

```cmake
juce_add_module(path/to/Juce-Equal-Loudness/modules/juce_equal_loudness)

target_link_libraries(MyPlugin PRIVATE juce_equal_loudness)
```

Then in your audio processor:

```cpp
#include <juce_equal_loudness/juce_equal_loudness.h>

juce_equal_loudness::EqualLoudnessProcessor loudness;

void prepareToPlay (double sampleRate, int samplesPerBlock) override
{
    loudness.prepare ({ sampleRate, (juce::uint32) samplesPerBlock,
                        (juce::uint32) getTotalNumOutputChannels() });
    loudness.setPhonLevel (60.0f);
}

void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
{
    loudness.process (buffer);
}
```

## Scope and limitations

This is a small first release. Known limitations:

- **No parameter smoothing.** Changing `setPhonLevel` during playback may
  produce an audible click as the biquad coefficients jump. Either change
  the parameter only between blocks from the message thread, or smooth it
  externally and call once per block.
- **One direction.** Applies the inverse equal-loudness contour. There is no
  "reference vs target" mode and no flatten/inverse switch.
- **`float` samples only.** `double` is not supported.
- **Fixed 8-biquad cascade.** No way to trade quality for CPU; no FIR mode.
- **Phon range 20–90.** ISO 226:2003 only validates the formula over this
  range. `setPhonLevel` clamps to it.
- **Large gains at low phon levels.** At 30 phon the bass boost required to
  invert the contour exceeds 25 dB. The processor applies the full gain; if
  you need it tame, add output gain compensation, a limiter, or restrict the
  phon range exposed in your UI.
- **Free-field contour only.** ISO 226 defines the free-field / diffuse-field
  contour; there is no separate headphone mode.

If any of these matter to you, open an issue.

## References

- ISO 226:2003 — Acoustics — Normal equal-loudness-level contours.
- Robinson and Dadson (1956) — original equal-loudness contour measurements.
- RBJ Audio EQ Cookbook — https://www.w3.org/TR/audio-eq-cookbook/

## License

MIT. See `LICENSE`.
