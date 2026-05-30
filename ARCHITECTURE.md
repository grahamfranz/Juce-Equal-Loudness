# Architecture

A short tour through how `juce_equal_loudness` is organised and why the few
design choices were made the way they were. The module is small on purpose —
the goal of this document is to make the small thing comprehensible, not to
make it look bigger than it is.

## File layout

```
modules/juce_equal_loudness/
    juce_equal_loudness.h     ← module declaration, ISO 226 data, public API
    juce_equal_loudness.cpp   ← single translation unit, all definitions
examples/
    demo.cpp                  ← console program
tests/
    iso226_tests.cpp          ← GoogleTest
CMakeLists.txt                ← JUCE_PATH-based build
```

The module follows the standard JUCE single-`.h` / single-`.cpp` layout. The
`BEGIN_JUCE_MODULE_DECLARATION` block at the top of the header is what
`juce_add_module` parses; there is no separate `juce_module_info` file.

## What the math is

ISO 226:2003 defines per-frequency coefficients `αf`, `Lu`, `Tf` at 29
reference frequencies and gives a closed-form expression for the sound
pressure level `Lp` on the equal-loudness contour at loudness level `Ln`:

```
A_f = 4.47e-3 * (10^(0.025 * Ln) - 1.15)
    + (0.4 * 10^((Tf + Lu)/10 - 9))^αf

Lp  = (10 / αf) * log10(A_f) - Lu + 94
```

The coefficient arrays are reproduced verbatim in `juce_equal_loudness.h`
inside the `ISO226` struct. For frequencies between the 29 reference points,
`αf`, `Lu`, and `Tf` are linearly interpolated in **log-frequency** space,
and the SPL is evaluated from the closed form on demand.

## What the filter does

`EqualLoudnessProcessor` builds a fixed 8-biquad cascade of RBJ peaking
filters. Design frequencies (in Hz) are:

```
50, 125, 250, 500, 2000, 4000, 6300, 10000
```

These are roughly log-spaced and deliberately skip 1 kHz — the gain there is
always 0 by construction, so the slot would be wasted on a no-op filter.

For each design frequency `f` and the current phon level `Ln`, the target
gain is:

```
gain(f) = ISO226::splAtFrequency(f, Ln) − Ln
```

i.e. the difference between the contour SPL at `f` and the SPL at 1 kHz
(which equals `Ln`). At low frequencies the gain is positive (boost), at the
ear's most sensitive region around 3–4 kHz it is mildly negative, and at
very high frequencies it becomes positive again as the contour rises.

The cascade is an approximation: each peaking filter contributes its own
local hump, and the resulting magnitude response only loosely matches the
exact analytic contour, especially at low phon levels where required gains
are large. `gainAtFrequency()` returns the **target** contour gain (computed
analytically), not the realised filter response. If you need the realised
response, evaluate the biquad transfer functions directly.

## Real-time safety

`prepare()` is the only function that allocates. It sizes the per-channel
biquad-state vector once, based on `ProcessSpec::numChannels`. Everything
else — `process()`, `reset()`, `setPhonLevel()` — touches only pre-allocated
storage and arithmetic, no heap.

The filter cascade is a fixed-size `std::array`, so changing the phon level
updates coefficients in place. Nothing resizes.

That said, `setPhonLevel()` is not parameter-smoothed: it overwrites all
biquad coefficients atomically per call, which causes a step change in the
filter response. The audible effect is a click. v0.1 leaves smoothing as the
caller's responsibility (drive the parameter from the message thread, or
ramp it externally and call once per block).

## Test strategy

Tests fall into two groups:

1. **ISO 226 reference data.** The 1 kHz column must equal the phon level
   exactly; a handful of well-known published contour points (e.g. 40 phon @
   100 Hz ≈ 64.4 dB) must match to within ~0.5 dB; the contour must be
   monotonic in phon; out-of-range frequencies must clamp and return finite
   values. These tests are tight and will catch any future regression in the
   coefficient tables or the formula.

2. **Processor behaviour.** Defaults, clamping, no crashes, no NaN/Inf in
   the output, `reset()` actually clears state. These are coverage tests
   rather than DSP correctness tests — they don't try to measure the filter
   response.

