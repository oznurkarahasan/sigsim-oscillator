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

## Phase 4 — GUI (ImGui + implot real-time scope): COMPLETE, verified visually

Installed `mingw-w64-ucrt-x86_64-glfw`. Vendored `third_party/imgui`
(`v1.89.9-docking`) and `third_party/implot` (`v0.16`) — pinned to that
specific pair deliberately: implot v0.16 doesn't compile against imgui's
current docking tip (imgui's internal API changed, e.g. `ImFont::FontSize`
renamed, `IM_OFFSETOF`/`IM_FLOOR` moved), so the versions must be matched
to the same era. Built:
- `include/detect/BiquadDetector.h` gained `lastBandpassOutput()` (not part
  of `IDetector`) so the scope can plot the raw bandpass waveform, not just
  the post-envelope magnitude.
- `src/gui/RingBuffer.h` — fixed-capacity circular buffer (standard ImPlot
  "ScrollingBuffer" idiom: plain array + offset, no per-push shifting).
- `src/gui/ScopeState.h` — owns `SignalChain` + all 3 detectors + their
  `BitScorer`s (with the same per-detector latency compensation as
  `detect_sweep`) + a `RollingBer` (trailing-200-symbol BER) + all 12 scope
  ring buffers; `advance()` is the only per-sample entry point.
- `src/gui/main.cpp` — GLFW/OpenGL3 bootstrap, wall-clock accumulator frame
  loop (fixed step at Fs, catch-up clamped to ~5 frames' worth of real
  time — not literally "5 samples", which would crawl at 200kHz), full
  control panel (every `Params`/`DetectorParams` field, grouped, Run/Pause/
  Reset, live rolling-BER readout), and the 6-subplot scope panel.
- `CMakeLists.txt`: `find_package(glfw3)`/`find_package(OpenGL)` gate the
  `sigsim_gui` target so the CLI tools keep building regardless.

### Real bug found and fixed: ImPlot axes don't continuously auto-fit inside a subplot grid

Verified via direct screenshot testing (`PrintWindow` capture, bypassing
window focus/z-order — this machine has other concurrent, unrelated
sessions active, which repeatedly interfered with naive full-screen
screenshots and even caused real slider-drag drift when the GUI window
happened to be under a stray cursor). Symptom: control panel and axis
labels/legends rendered correctly, and direct instrumentation confirmed
the ring buffers held fully correct, continuously-growing real data — but
no line was ever visible on any of the 6 plots, and the X-axis stayed
stuck near its very-first-frame range instead of tracking the scrolling
window.

Root cause: without `ImPlotAxisFlags_AutoFit` explicitly passed to
`ImPlot::SetupAxes()` on every `BeginPlot`, axes *inside a subplot grid*
only fit once (against whichever frame first established that axis,
often before real data existed) and then hold that range forever — real,
correct, continuously-scrolling data silently falls outside a fixed,
stale view. Isolated by comparing one plot with the flag against five
without it, fed identical data side by side in the same window. Fixed by
adding `ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit,
ImPlotAxisFlags_AutoFit)` as the first call inside every `BeginPlot` block.

Verified visually afterward (`PrintWindow` capture): all 6 subplots render
live, correct, continuously-scrolling data — ADC input (adc_v vs ideal_v),
biquad bandpass + envelope, Goertzel step plot, autocorrelation magnitude,
fade multiplier, and the 4-row stacked bit-decision plot all confirmed.

### Not yet done for Phase 4

Interactive acceptance pass beyond the one screenshot round: confirming
each control-panel slider visibly changes its subplot within one frame,
Run/Pause/Reset behavior under real interaction, and a CPU-usage check
specifically on autocorrelation (confirm the O(1)-per-sample sliding-
window implementation, not an O(W) recompute, holds up live at
200k samples/sec) — deferred because reliable interactive testing on this
shared machine needs a real mouse, not the screenshot-only automation used
so far.

## Not yet done

- **Phase 5** — packaging: README update to remove completed items from
  "Not in this deliverable", final smoke-test sequence, zip.
