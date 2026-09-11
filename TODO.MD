# Session summary — environment fix + Phase 3 (detectors)

## Environment fixes (blocking everything else)

- **Build crash (Release only)**: `sigsim_dump.exe` segfaulted immediately
  (even `--duration-ms 0`) at `-O1` and above. Root cause: dynamic
  libstdc++/libgcc resolved to Git for Windows' bundled MinGW DLL (first on
  `PATH`) instead of the compiling UCRT64 toolchain's own — an ABI
  mismatch. Fixed permanently in `CMakeLists.txt` via
  `-static-libgcc -static-libstdc++` on every executable target (see
  `sigsim_harden_mingw()` helper), so it no longer depends on `PATH` order.
- **`tools/verify.py` couldn't run**: no `pip`/`numpy` on the MSYS2 UCRT64
  Python. Installed via `pacman -S mingw-w64-ucrt-x86_64-python-numpy`.
- Confirmed: clean build, `tools/verify.py` (original 6 checks) all PASS.

## Development plan review (`developmentplan.md`)

Assessed as sound and well-scoped for Phases 3–5: correctly builds on
`SignalChain::step()`/`AdcSample`, reuses `AnalogFrontEnd`'s exact
discretization, matches the README's "headless-first" verification
philosophy, and the autocorrelation fractional-lag fix is technically
correct in principle. Flagged then, confirmed during implementation: the
guard-band scoring design (§3.7) doesn't actually work when a detector's
block/window length equals the symbol length (see bugs below).

## Phase 3 — Detectors: COMPLETE, all 16 verify.py checks PASS

Built per the plan's §3.1–3.8:
- `include/detect/DetectorTypes.h`, `IDetector.h`, `PeakTracker.h`,
  `BiquadDetector.h`, `GoertzelDetector.h`, `AutocorrDetector.h`,
  `BitScorer.h`
- `src/detect_dump.cpp` (CLI dump, all 3 detectors + ground truth)
- `src/detect_sweep.cpp` (headless BER-vs-noise/fade sweep — the actual
  comparison deliverable)
- CMake: `sigsim_detect` interface lib + `detect_dump`/`detect_sweep`
  targets, zero warnings
- `tools/verify.py` extended with checks 7–10 (§3.9 acceptance criteria)

### Real bugs found and fixed (not in the original plan)

1. **Missing AC-coupling.** `AdcSample::adcV` carries a ~1.6V DC offset next
   to a ~20mV carrier. Goertzel and autocorrelation both correlated/binned
   the raw voltage, so the offset dominated regardless of whether the
   carrier was present — autocorrelation read ~0.99 correlation even with
   noise-only input. Biquad was unaffected (its own bandpass response
   rejects DC by construction). Fix: AC-couple both detectors' inputs by
   subtracting a slow `AnalogFrontEnd` low-pass estimate of the offset
   before processing (same class already used elsewhere in the project).

2. **Guard band couldn't absorb full-symbol latency.** `BitScorer`'s 20%
   guard trim (§3.7) assumes detector latency is a small fraction of a
   symbol. But Goertzel's block-hold reveals a block's result only at the
   START of the NEXT symbol (a full symbol late, deterministically), and
   autocorrelation's window has similar smearing. Without compensation,
   Goertzel/autocorrelation scored near-chance BER *independent of noise
   level* — a wiring bug, not physics. Fix: `BitScorer::configure()` gained
   a `truthDelaySamples` parameter; `detect_sweep` computes each detector's
   own known latency (`GoertzelDetector::blockSamples()`,
   `AutocorrDetector::windowSamples()/2`) and feeds it in, so the guard
   band is left to handle only residual jitter as originally intended.

3. **Autocorrelation window length vs. OOK gap.** The plan's default
   `autocorrWindowMs = 1.0` ties the window 1:1 to the symbol length —
   but a window that long can never fully flush a preceding on-pulse's
   carrier before the next on-pulse arrives (verified: pure noise input
   settles to ~0 correlation once the window fills, but during real OOK
   cycling the window is never clean). Changed the default to `0.25`ms;
   documented the reasoning directly in `DetectorTypes.h`.

4. **`PeakTracker` rate bug (the big one).** `PeakTracker::decay_` is
   calibrated assuming `update()` is called once per sample at
   `sampleRateHz`. `GoertzelDetector` only calls it once per BLOCK (every
   `blockN_` samples), since `rawMagnitude` only changes that often. This
   silently made the configured `peakTauMs` (20ms default) effectively
   `blockN_`x too slow in wall-clock terms — with the default 200-sample
   block, a nominal 20ms tau became ~4 SECONDS. A single inflated startup
   block (before the signal chain settles) would then corrupt the AGC
   normalization for the entire run, pinning `normalizedLevel` right at
   the 0.5 threshold regardless of true signal level. Fix:
   `peak_.configure(p_.peakTauMs, p_.sampleRateHz / blockN_)` — calibrate
   against the actual call rate, not the raw sample rate.

5. **Autocorrelation's OWN fractional-lag fix was mathematically wrong.**
   The plan's linear interpolation `(1-frac)*r(lagLo) + frac*r(lagHi)`
   underestimates the true correlation peak, because r(L) vs. lag is
   cosine-shaped (concave near its peak) for a near-monochromatic signal —
   a linear blend is a chord under that arc. Verified both analytically
   and empirically: at this project's own default 47.4kHz/200kHz ratio
   (frac=0.219), the linear-interpolated result (0.826) scored WORSE than
   plain nearest-integer-lag rounding (0.947) — the opposite of what the
   fix is supposed to do. Fixed with an exact closed-form two-point
   amplitude/phase reconstruction (the target frequency, hence omega, is
   known exactly, so the two lag correlations fully determine the true
   peak via `rho = sqrt(rLo^2 + ((rLo*cos(omega) - rHi)/sin(omega))^2)`).
   Now scores 1.000 vs. integer-lag's 0.947, as intended.

6. **My own test-harness wiring bug** (not shipped, caught before it
   mattered): `detect_dump --sig-freq` was also silently retuning the
   detector's own `targetFreqHz` to match, so "off-target tone" tests were
   accidentally always on-target. Added an independent `--target-freq-hz`
   flag.

### Verified sweep behavior (defaults, `detect_sweep`)

At `noise-mv=10` (easy): biquad 0%, goertzel 0.05%, autocorr 0% BER.
At `noise-mv=4000` (very hard): biquad 45.4%, goertzel 29.2%, autocorr
47.6% BER — near chance, confirming genuine noise-driven degradation, not
a wiring artifact. (Plan's illustrative "noise-mv=200 → BER>20%" turned out
too easy for a correctly-working implementation — all three detectors get
real processing gain from integrating over many carrier cycles against a
peak-normalized threshold, documented in `verify.py`'s docstring.)

## Not yet done

- **Phase 4** — ImGui + implot real-time GUI scope (needs GLFW3/OpenGL dev
  packages + a display; can't be verified headlessly per the plan itself).
- **Phase 5** — packaging: README update to remove completed items from
  "Not in this deliverable", final smoke-test sequence, zip.
