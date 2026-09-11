# 47 kHz Signal-Chain Simulator + Detector Test Bench

Headless C++ simulation of the analog signal path that a 47 kHz OOK
receiver's ADC would actually see: carrier + white noise + DC offset,
through a single-pole RC anti-alias filter, through an N-bit ADC
quantizer, with a slowly-fading amplitude envelope and a 500-baud-style
on/off test modulation — plus three independent bit detectors (biquad
bandpass, Goertzel, autocorrelation) scored against ground truth. This is
deliberately **signal source + detectors, no GUI yet**. Everything is
verified from the command line first (`tools/verify.py`), so correctness
doesn't depend on Dear ImGui/implot ever getting wired up (that's the next
phase — see [Not in this deliverable](#not-in-this-deliverable)).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Produces three CLIs:

| Binary | Purpose |
|---|---|
| `build/sigsim_dump` | Dumps the raw signal chain (no detectors) as CSV |
| `build/detect_dump` | Dumps the signal chain **and** all three detectors' output per sample, as CSV |
| `build/detect_sweep` | Headless BER-vs-parameter sweep — the actual biquad/Goertzel/autocorrelation comparison |

```
./build/sigsim_dump --help
./build/detect_dump --help
./build/detect_sweep --help
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
that detectors are scored against.

Example: compare all three detectors' bit-error-rate across a noise sweep:

```
./build/detect_sweep --sweep-param noise-mv --values 10,15,20,25,30,35,40,50,60,80 \
    --symbols-per-point 2000 --seed 1 --out sweep.csv
```

produces `sweep_value,ber_biquad,ber_goertzel,ber_autocorr` — one row per
swept value. `--sweep-param` also accepts `fade-max`.

On MinGW toolchains, every binary is statically linked against
libstdc++/libgcc (`CMakeLists.txt`'s `sigsim_harden_mingw()`), so it
doesn't depend on which MinGW distribution's DLLs happen to be first on
`PATH` at runtime — a machine with more than one MinGW install (e.g. Git
for Windows' bundled toolchain ahead of the one that compiled the binary)
can otherwise load an ABI-incompatible libstdc++ and crash immediately.

## Verify

```
python3 tools/verify.py
```

Runs the built binaries through 16 checks and reports PASS/FAIL for each:

- **1–6, signal chain**: carrier frequency accuracy, noise RMS calibration,
  ADC clipping/LSB size, the RC filter's -3dB point, and (the important
  one) that noise between Fs/2 and the RC cutoff actually aliases back into
  the decimated output when the cutoff is set above Fs/2, and OOK symbol
  timing.
- **7–10, detectors**: biquad's analytic RBJ magnitude response (on- and
  off-target), Goertzel's on-target convergence and off-target suppression,
  the autocorrelation fractional-lag fix actually outperforming naive
  integer-lag rounding, and `detect_sweep`'s BER trending with noise
  (not stuck at a constant value regardless of noise — see
  [Detector implementation notes](#detector-implementation-notes) for why
  that distinction mattered).

Needs `numpy` (`pip install numpy`, or on MSYS2/MinGW,
`pacman -S mingw-w64-ucrt-x86_64-python-numpy` — whichever `python3`
resolves to needs to be the one numpy was installed into).

Current status: **all 16 checks pass.**

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
| `tools/verify.py` | Headless correctness checks against the built binaries |

Detectors, each consuming only `AdcSample::adcV` (never the ground-truth
fields), tied together the same way:

```
adcV ──▶ [ Biquad | Goertzel | Autocorrelation ] ──▶ PeakTracker (AGC) ──▶ bitDecision
```

| File | Responsibility |
|---|---|
| `include/detect/DetectorTypes.h` | `DetectorOutput` (per-sample result) and `DetectorParams` (every detector tunable) |
| `include/detect/IDetector.h` | Common interface; `configure()` is safely re-callable at runtime |
| `include/detect/PeakTracker.h` | Shared AGC/peak-follower: normalizes each detector's own-units magnitude to a 0..1-ish level a single shared `thresholdFrac` can be applied to |
| `include/detect/BiquadDetector.h` | RBJ constant-0dB-peak-gain bandpass → rectify → envelope (reuses `AnalogFrontEnd`) |
| `include/detect/GoertzelDetector.h` | Block-based single-bin power, block length tied to the OOK symbol length |
| `include/detect/AutocorrDetector.h` | Fractional-lag autocorrelation, O(1)-per-sample sliding window |
| `include/detect/BitScorer.h` | Per-symbol majority-vote BER scoring against ground truth, with guard band + per-detector latency compensation |
| `src/detect_dump.cpp` | CLI: signal chain + all three detectors, dumps CSV |
| `src/detect_sweep.cpp` | CLI: BER-vs-parameter sweep, dumps CSV — the actual comparison deliverable |

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

## Detector implementation notes

A few things the detectors get right that aren't obvious from the
formulas alone, each verified in `tools/verify.py` checks 7–10:

**AC-coupling.** `AdcSample::adcV` carries a ~1.6V DC offset next to a
~20mV carrier. Goertzel and autocorrelation both bin/correlate their raw
input, so without removing that offset first, it dominates regardless of
whether the carrier is actually present (autocorrelation in particular
would read ~0.99 correlation on pure noise). Both detectors AC-couple
their input by subtracting a slow `AnalogFrontEnd` low-pass estimate of
the offset before processing — biquad doesn't need this, its own bandpass
response already rejects DC by construction.

**Guard-band latency compensation.** `BitScorer`'s guard band (trimming
the first/last 20% of a symbol from the majority vote) only covers small
edge jitter — it can't absorb a detector's *systematic* decision latency
when that latency is comparable to a full symbol. Goertzel's block-hold
value is revealed only at the start of the *next* symbol (a full block
late, deterministically); autocorrelation's sliding window has similar
smearing. `BitScorer::configure()` takes a `truthDelaySamples` parameter
so each detector's own known latency (`GoertzelDetector::blockSamples()`,
`AutocorrDetector::windowSamples()/2`) is compensated before the guard
band ever runs — without this, Goertzel/autocorrelation score
near-chance BER *independent of noise level*, which is a wiring bug, not
physics.

**Autocorrelation window length.** Deliberately *not* tied 1:1 to the OOK
symbol length the way `goertzelBlockMs` is (default `autocorrWindowMs =
0.25`, a quarter of the 1ms symbol). A sliding window as long as the
off-gap itself can never fully flush the previous on-pulse's carrier
before the next on-pulse arrives — verified empirically by feeding pure
noise (correlation correctly settles to ~0 once the window fills) versus
real OOK cycling with a 1ms window (correlation stays elevated through
nearly the entire off period, because the window legitimately still
contains real carrier samples — not a bug, just too long a memory for the
gap it has to forget within).

**`PeakTracker`'s decay is calibrated to the caller's actual update
rate, not the raw sample rate.** Goertzel calls `PeakTracker::update()`
once per *block* (every `blockN_` samples), not once per sample, since
`rawMagnitude` only changes that often. Configuring the tracker at the
raw `sampleRateHz` would make the configured `peakTauMs` `blockN_` times
too slow in wall-clock terms — with the default 200-sample block, a
nominal 20ms tau silently becomes ~4 seconds, so one inflated startup
block (before the signal chain settles) corrupts AGC normalization for
the entire run. `GoertzelDetector` configures its tracker against
`sampleRateHz / blockN_` instead.

**The fractional-lag fix needed its own fix.** Linearly interpolating
`(1-frac)*r(lagLo) + frac*r(lagHi)` between the two nearest integer lags
*underestimates* the true correlation peak, because correlation-vs-lag is
cosine-shaped (concave near its peak) for a near-monochromatic signal — a
linear blend is a chord under that arc. At this project's own default
47.4kHz/200kHz ratio (`frac=0.219`), that linear blend actually scored
*worse* than plain nearest-integer-lag rounding — the opposite of the
fix's purpose. Since the target frequency (hence its phase increment
`omega`) is known exactly, `AutocorrDetector` instead solves for the true
peak analytically from the two measured lag correlations:
`rho = sqrt(rLo^2 + ((rLo*cos(omega) - rHi)/sin(omega))^2)`.

**`detect_sweep`'s BER-vs-noise sanity numbers.** At `noise-mv=10` (easy),
all three detectors score under 1% BER. At `noise-mv=4000` (very hard),
all three approach chance level (~30–48%) — deliberately not the
`noise-mv=200` figure one might expect from "signal buried below noise":
all three detectors get real processing gain from integrating over many
carrier cycles (envelope/block/window) against a peak-normalized
threshold, so 200mV of noise (10x the 20mV carrier) isn't actually enough
to drive a correctly-working implementation to chance level.

## Not in this deliverable

The ImGui/implot real-time scope (GUI) is the next phase, built on top of
`SignalChain::step()` and the `IDetector` implementations here — signal
chain, all three detectors, and the headless BER comparison harness
(`detect_sweep`) are already in place and verified.
