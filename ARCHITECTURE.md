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

### Iterative residual correction

A naive design — sizing each peaking filter independently against its target
gain — overshoots between design frequencies because adjacent filters'
skirts add together, and the crosstalk compounds: e.g. at 60 phon the 250 Hz
target is +7.5 dB, but if each band is fit independently the realised
response there sits around +17 dB.

Instead, the cascade is designed iteratively. Each pass:

1. Evaluate the realised cascade magnitude at every design frequency.
2. Take the residual (target − realised).
3. Update each filter's gain by a damped fraction of its residual.
4. Redesign all filters with the new gains.

Damping is 0.5 to avoid oscillation when target gains are large; convergence
to <0.05 dB max residual usually takes 6–12 passes and is capped at 20.
This is all done in `prepare()` and `setPhonLevel()`, never on the audio
thread.

A regression test asserts the realised response at the 8 design frequencies
sits within 1.5 dB of the analytic target across the 30/60/90 phon range.

Off-design frequencies still drift — the cascade has only 8 degrees of
freedom, so the response between adjacent designed bands is whatever the
neighbouring filters' shoulders produce. `gainAtFrequency()` returns the
**target** contour gain (computed analytically), not the realised filter
response.

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

Tests fall into three groups:

1. **ISO 226 reference data.** The 1 kHz column must equal the phon level
   (modulo the formula's intrinsic ~0.05 dB residue); a handful of published
   contour points must match to within ~0.5 dB; the contour must be
   monotonic in phon; out-of-range frequencies must clamp and return finite
   values. These tests catch regressions in the coefficient tables or the
   closed-form expression.

2. **Cascade fit.** Drives a sine through the processor at each of the 8
   design frequencies across 30/60/90 phon, measures the realised gain by
   RMS, and asserts it sits within 1.5 dB of the analytic target. This is
   the regression guard for the iterative design loop: any change that
   silently regresses to naive per-band fitting will fail loudly here.

3. **Processor behaviour.** Defaults, clamping, no crashes, no NaN/Inf in
   the output, `reset()` actually clears state. Coverage rather than DSP
   correctness.

