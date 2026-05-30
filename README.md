# juce_equal_loudness

A small JUCE module that applies ISO 226:2003 equal-loudness compensation as a
real-time IIR peaking-filter cascade.

**Status: v0.1.** Single mode, single direction, no parameter smoothing. See
[Scope and limitations](#scope-and-limitations) before depending on it.

## What it does

Given a phon level `Ln`, the processor builds an 8-band peaking-filter cascade
whose magnitude response approximates the inverse of the ISO 226:2003
equal-loudness contour at `Ln`, normalised to 0 dB at 1 kHz. Audio passed
through this filter is shaped so that flat-spectrum input is perceived as
spectrally balanced by a listener whose ear is operating at that phon level.

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

This is deliberately a small first release. Things it explicitly does not do:

- **No parameter smoothing.** Changing `setPhonLevel` during playback may
  produce an audible click as the biquad coefficients jump. Either change the
  parameter only between blocks from the message thread, or smooth it
  externally and call once per block.
- **One direction.** Applies the inverse equal-loudness contour. There is no
  "reference vs target" mode and no flatten/inverse switch.
- **One sample type.** `float` only.
- **Eight biquads, fixed.** No way to trade quality for CPU; no FIR mode.
- **Phon range 20–90.** The ISO 226:2003 formula is only validated over this
  range. `setPhonLevel` clamps to it.
- **Large gains at low phon levels.** At e.g. 30 phon, the bass boost
  required to invert the contour exceeds 25 dB. The cascade applies it
  honestly; if you need it tame, use it with output gain compensation or a
  limiter, or restrict the phon range your UI exposes.
- **No headphone vs free-field option.** ISO 226 is the diffuse-field /
  free-field standard contour.

If any of these matter to you, file an issue and the design conversation can
happen in the open.

## References

- ISO 226:2003 — Acoustics — Normal equal-loudness-level contours.
- Robinson and Dadson (1956) — original equal-loudness contour measurements.
- RBJ Audio EQ Cookbook — https://www.w3.org/TR/audio-eq-cookbook/

## License

MIT. See `LICENSE`.
