# 47 kHz Signal-Chain Simulator

Headless C++ simulation of the analog signal path that a 47 kHz OOK
receiver's ADC would actually see: carrier + white noise + DC offset,
through a single-pole RC anti-alias filter, through an N-bit ADC
quantizer, with a slowly-fading amplitude envelope and a 500-baud-style
on/off test modulation. This is deliberately **just the signal source /
front end** — no detectors, no GUI. It's the ground truth generator that
detector code (biquad / Goertzel / autocorrelation) gets built against
next, and it's meant to be trusted before that happens.

No GUI dependency by design: everything is verified from the command
line first (`tools/verify.py`), so correctness doesn't depend on Dear
ImGui/implot ever getting wired up.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Produces `build/sigsim_dump`, a CLI that dumps the signal chain as CSV.

```
./build/sigsim_dump --help
```

Example: default parameters, 25ms, to a file:

```
./build/sigsim_dump --duration-ms 25 --out run.csv
```

CSV columns: `time_s, ideal_v, pre_adc_v, adc_code, adc_v, bit_truth, fade_mult`
— `ideal_v` is the noiseless/unfiltered carrier+offset (ground truth),
`pre_adc_v` is what the ADC pin actually sees (post-noise, post-RC,
pre-quantization), `adc_code`/`adc_v` are the quantized reading, and
`bit_truth`/`fade_mult` are the ground-truth OOK bit and fade multiplier
for later scoring a detector's bit-error-rate against.

## Verify

```
python3 tools/verify.py
```

Runs the binary through six checks and reports PASS/FAIL for each —
carrier frequency accuracy, noise RMS calibration, ADC clipping/LSB
size, the RC filter's -3dB point, and (the important one) that noise
between Fs/2 and the RC cutoff actually aliases back into the decimated
output when the cutoff is set above Fs/2. Needs `numpy` (`pip install
numpy --break-system-packages` if missing).

Current status: **all 6 checks pass.**

## Architecture

```
Oscillator ──▶ ×amplitude(fade, OOK gate) ──▶ + offset ──▶ + noise
   ──▶ RC low-pass (run at oversampled rate) ──▶ decimate to Fs ──▶ clip + quantize (ADC)
```

One class per stage, each independently testable, tied together by
`SignalChain`:

| File | Responsibility |
|---|---|
| `include/sigsim/Types.h` | `Params` (every tunable) and `AdcSample` (per-sample output + ground truth) |
| `include/sigsim/Oscillator.h` | Phase-accumulator sine generator — frequency can change live without a phase click |
| `include/sigsim/NoiseGenerator.h` | White Gaussian noise, calibrated to an RMS in mV |
| `include/sigsim/FadeEnvelope.h` | Slow random-walk amplitude multiplier (source moving near/far, including hard clipping) |
| `include/sigsim/OokGate.h` | 500-baud-style on/off test modulation (default 1ms on / 1ms off) |
| `include/sigsim/AnalogFrontEnd.h` | Single-pole RC low-pass, exact discretization (see below) |
| `include/sigsim/AdcQuantizer.h` | Clip to [0, Vref], quantize to N bits |
| `include/sigsim/SignalChain.h` | Wires the above together; `step()` is the only entry point the rest of the project needs |
| `src/main.cpp` | CLI harness: parses `Params` from flags, dumps CSV |
| `tools/verify.py` | Headless correctness checks against the built binary |

### Why oversampling + decimation, not filtering directly at Fs

The RC filter and noise generator both run at `Fs × oversample` (default
16×, i.e. 3.2 MHz for the 200 kHz default). Only the *last* internal tick
of each block of `oversample` ticks is kept as the ADC-rate sample — that
decimation step has no extra filtering attached to it.

This matters because a discrete filter can only remove energy that
exists in the sequence it's applied to. If noise were generated directly
at Fs and filtered at Fs, it would already be band-limited to Fs/2 by
construction — no filter cutoff setting could ever demonstrate aliasing,
because there'd be nothing above Fs/2 to fold back. Real hardware
aliases because the RC filter is analog (continuous-time) and the ADC
samples *after* it. Running the filter+noise at an oversampled rate and
decimating without any further filtering reproduces that: energy the RC
filter didn't remove between Fs/2 and the oversampled Nyquist genuinely
folds into the working band after decimation, exactly like a real board
with an inadequate anti-alias filter. `tools/verify.py` check 5 confirms
this quantitatively (measured post-decimation noise RMS matches an AR(1)
prediction of the filter's actual passband, not a naive "filtered at Fs"
assumption).

### RC filter discretization

`AnalogFrontEnd` uses the *exact* zero-order-hold discretization of the
RC differential equation, `alpha = 1 - exp(-dt/RC)`, not the more common
`dt/(RC+dt)` approximation. The two only agree when `dt << RC`; once the
cutoff gets within an order of magnitude of the oversampled tick rate
(which happens whenever someone pushes the cutoff slider up toward Fs),
the approximation warps badly — an earlier version of this code used it
and `tools/verify.py` caught the resulting ~30% RMS error before
anything downstream depended on it.

### Fade envelope

The random walk happens in a normalized [0,1] space, updated at a slow
keyframe rate (`fadeUpdateMs`, default 50ms) and linearly interpolated
between keyframes so the value fed to the carrier every tick is
continuous, not a staircase. The normalized value is mapped
*geometrically* (log-linear) onto `[fadeMinMult, fadeMaxMult]`, because a
signal source's amplitude falling off with distance is a multiplicative
process, not an additive one. Default range (0.05× to 120×) is wide
enough to swing from near-noise-floor up past hard clipping.

## Known parameter caveat: default RC cutoff

The default `--rc-cutoff` (100 kHz) sits close to Fs/2 (100 kHz for the
default 200 kHz sample rate) and doesn't attenuate much in-band — it's a
deliberately "leaky" anti-alias filter, representative of a cheap analog
front end, not a well-designed one. If you want to see detectors working
against a *properly* anti-aliased signal, drop it toward 40-80 kHz; to
deliberately stress-test detectors against aliased noise, push it up
toward or above 100 kHz. Both are meaningful test conditions — the point
of exposing it as a live parameter is to compare them, not to hide a bad
default.

## Not in this deliverable

Detectors (biquad bandpass, Goertzel, autocorrelation) and the
ImGui/implot real-time scope are the next phase, built on top of
`SignalChain::step()`. This zip is the front-end simulation only, per
your last request.
