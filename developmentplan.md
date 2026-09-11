# 47 kHz Detector Test Bench — Development Plan, Phases 3–5 {#47-khz-detector-test-bench-development-plan-phases-35}

Phases 1–2 are complete and delivered (`sigsim-oscillator.zip`): a CMake project with a headless, GUI\-free signal\-chain simulator (`include/sigsim/*.h`, `src/main.cpp`, `tools/verify.py`), all correctness checks passing. This plan specifies Phase 3 (three detectors \+ a scoring harness), Phase 4 (ImGui/implot real\-time scope), and Phase 5 (build/packaging), precisely enough to hand to a coding agent without further clarification. Defaults given below are decisions, not suggestions — implement them as specified unless a section explicitly flags a choice.

## Starting point: what Phase 3 builds on {#starting-point-what-phase-3-builds-on}

Existing repo layout:

```
sigsim-oscillator/
  CMakeLists.txt
  include/sigsim/
    Types.h            Params, AdcSample
    Oscillator.h
    NoiseGenerator.h
    FadeEnvelope.h
    OokGate.h
    AnalogFrontEnd.h
    AdcQuantizer.h
    SignalChain.h
  src/main.cpp         CLI dump harness (sigsim_dump)
  tools/verify.py       headless correctness checks
```

The only entry point Phase 3 code needs is `sigsim::SignalChain::step()`, returning one `AdcSample` per call:

```cpp
struct AdcSample {
    double   timeS;      // seconds, monotonic
    double   idealV;     // ground truth: carrier+offset, no noise/filter
    double   preAdcV;    // post-noise, post-RC, pre-quantization
    uint32_t code;        // raw ADC code
    double   adcV;        // quantized reading — THIS is what a detector consumes
    bool     bitTruth;    // ground truth OOK bit
    double   fadeMult;    // ground truth amplitude multiplier
};
```

Detectors consume `s.adcV` only. `idealV`, `bitTruth`, `fadeMult` are ground truth for scoring, never fed to a detector.

## Phase 3 — Detectors {#phase-3-detectors}

### 3\.1 Directory layout {#31-directory-layout}

```
include/detect/
  DetectorTypes.h     DetectorOutput, DetectorParams
  IDetector.h          abstract interface
  PeakTracker.h        shared AGC/peak-follower used by all three
  BiquadDetector.h
  GoertzelDetector.h
  AutocorrDetector.h
  BitScorer.h          per-symbol majority-vote scoring against ground truth
src/detect_dump.cpp    CLI: runs SignalChain + all three detectors, dumps CSV
src/detect_sweep.cpp   CLI: BER-vs-parameter sweep, dumps CSV (headless — this is the actual comparison deliverable)
```

### 3\.2 Common interface {#32-common-interface}

```cpp
// DetectorTypes.h
struct DetectorOutput {
    double rawMagnitude   = 0.0;  // detector-native units: volts (biquad), power (Goertzel), correlation coeff (autocorr)
    double normalizedLevel = 0.0; // rawMagnitude / tracked peak — dimensionless, comparable across detectors
    bool   bitDecision     = false; // normalizedLevel > thresholdFrac
};

struct DetectorParams {
    double sampleRateHz  = 200000.0; // must match sigsim::Params::sampleRateHz
    double targetFreqHz  = 47400.0;  // nominal carrier — must match sigsim::Params::signalFreqHz
    double thresholdFrac = 0.5;      // shared fractional threshold, applied to each detector's OWN peak (see 3.5)
    double peakTauMs      = 20.0;    // peak-tracker decay time constant

    // detector-specific, ignored by the other two:
    double biquadQ         = 8.0;
    double biquadEnvTauMs  = 0.3;    // envelope-follower smoothing after rectification
    double goertzelBlockMs = 1.0;    // block length — tie to the OOK symbol length (default 1ms)
    double autocorrWindowMs = 1.0;   // correlation window — tie to the OOK symbol length
};

// IDetector.h
class IDetector {
public:
    virtual void configure(const DetectorParams&) = 0;
    virtual void reset() = 0;
    virtual DetectorOutput processSample(double adcVolts) = 0; // called once per SignalChain::step()
    virtual const char* name() const = 0;
    virtual ~IDetector() = default;
};
```

`configure()` must be callable again at runtime with new parameters (the GUI reconfigures on slider change — see 4.5); it should not require reconstructing the object.

### 3\.3 Peak tracker / AGC (shared component, `PeakTracker.h`) {#33-peak-tracker-agc-shared-component-peaktrackerh}

Every detector owns one instance. It converts a detector's raw, unit\-specific magnitude into a normalized 0..1\-ish level that a single shared `thresholdFrac` can be applied to, and lets all three track the fading amplitude envelope instead of using a fixed voltage.

```cpp
class PeakTracker {
public:
    void configure(double tauMs, double sampleRateHz) {
        decay_ = std::exp(-1.0 / (sampleRateHz * (tauMs / 1000.0)));
    }
    double update(double rawMagnitude) {
        peak_ = std::max(rawMagnitude, peak_ * decay_);
        return rawMagnitude / std::max(peak_, 1e-12);
    }
    void reset() { peak_ = 0.0; }
private:
    double decay_ = 0.999;
    double peak_  = 0.0;
};
```

`peakTauMs` default 20ms: long enough to not track individual noise spikes (bit period is 1ms), short enough to follow the fade envelope's 50ms keyframe rate with some lag. This is a tuning knob, not a fixed constant — expose it in `DetectorParams` and, later, the GUI.

### 3\.4 Biquad bandpass detector {#34-biquad-bandpass-detector}

RBJ cookbook constant\-0dB\-peak\-gain bandpass, rectifier, envelope follower, peak tracker.

```
w0    = 2*pi*targetFreqHz / sampleRateHz
alpha = sin(w0) / (2*Q)

b0 =  alpha
b1 =  0
b2 = -alpha
a0 =  1 + alpha
a1 = -2*cos(w0)
a2 =  1 - alpha
```

Normalize all coefficients by `a0` before use (standard Direct Form I or II biquad). Per sample:

```
y        = biquad(x)              // bandpass output
rectified = fabs(y)
envelope  = onePoleLowpass(rectified, tau = biquadEnvTauMs)   // exact discretization, see AnalogFrontEnd.h — REUSE that class, don't reimplement
rawMagnitude = envelope
```

Reuse `sigsim::AnalogFrontEnd` for the envelope follower rather than writing a second one\-pole filter implementation — same exact\-exponential math applies.

`biquadEnvTauMs` default 0.3ms: fast enough to resolve the 1ms bit period (about 3 time constants per bit — reaches \~95% of final value), slow enough to smooth the rectified carrier ripple (carrier period ≈ 21µs at 47.4kHz, so tau\=0.3ms averages over \~14 carrier cycles).

### 3\.5 Goertzel detector {#35-goertzel-detector}

Block\-based single\-bin power, block length tied to the OOK symbol duration so one block ≈ one bit decision.

```
N = round(sampleRateHz * goertzelBlockMs / 1000)     // default: 200 samples at 200kHz/1ms
omega = 2*pi*targetFreqHz / sampleRateHz
coeff = 2*cos(omega)                                  // NOT tied to an integer bin — Goertzel works for
                                                        // any target frequency at a given block length,
                                                        // this is what makes it robust to the carrier
                                                        // not being exactly 47kHz

// per block of N samples:
s1 = s2 = 0
for each sample x in block:
    s0 = x + coeff*s1 - s2
    s2 = s1
    s1 = s0
power = s1*s1 + s2*s2 - coeff*s1*s2
rawMagnitude = sqrt(power) / N            // normalize by block length so magnitude is amplitude-like, comparable across block sizes
```

`processSample()` accumulates into the current block and only updates `rawMagnitude` (and therefore `bitDecision`) once every N samples — hold the previous value in between. Document this latency (up to `goertzelBlockMs`) explicitly in the class comment; it matters for the scorer (3.7).

### 3\.6 Autocorrelation detector — fractional lag (this is the fix, not the original ChatGPT design) {#36-autocorrelation-detector-fractional-lag-this-is-the-fix-not-the-original-chatgpt-design}

`sampleRateHz / targetFreqHz` is not an integer (200000/47400 \= 4.219). An integer\-lag autocorrelation at `round(fs/f0) = 4` actually resonates at `fs/4 = 50kHz`, 5.5% off target — over a 1ms bit (\~47 carrier cycles) this phase\-slips enough to significantly degrade the correlation peak. Use two\-tap linear interpolation between the adjacent integer lags instead of rounding:

```
lagF  = sampleRateHz / targetFreqHz         // e.g. 4.219
lagLo = floor(lagF)                          // 4
lagHi = lagLo + 1                            // 5
frac  = lagF - lagLo                         // 0.219

W = round(sampleRateHz * autocorrWindowMs / 1000)   // window length, default 200 samples (1ms)

// maintain a ring buffer of at least the last (W + lagHi) samples
r(L) = sum_{n=0}^{W-1} x[n] * x[n-L]          // over the window, x[n-L] read from the ring buffer
norm  = sqrt( sum_{n=0}^{W-1} x[n]^2 * sum_{n=0}^{W-1} x[n-L]^2 )   // per-lag normalization, or a single
                                                                       // running-energy normalization — either
                                                                       // is fine as long as it's consistent

rawMagnitude = ((1-frac) * r(lagLo) + frac * r(lagHi)) / norm   // normalized correlation coefficient, interpolated to the fractional lag
```

Recompute `r(L)` every sample using a sliding\-window sum update (add the new term, subtract the term leaving the window) rather than resumming the whole window each sample — this is an O(1)\-per\-sample update, not O(W). This detector is the most compute\-heavy of the three either way; do not additionally make it O(W) per sample.

If, later, the target frequency itself needs to be treated as unknown/drifting beyond the fixed nominal `targetFreqHz`, extend this to a small local search (lagLo\-1 .. lagHi\+1) with parabolic peak interpolation. Not required for this phase — the fixed two\-tap interpolation is sufficient for a carrier that's simply "not exactly 47kHz" around a known nominal.

### 3\.7 Bit scoring (`BitScorer.h`) {#37-bit-scoring-bitscorerh}

Per\-sample `bitDecision` is noisy at symbol edges and lags behind ground truth by each detector's own latency (biquad envelope settling, Goertzel's block latency, autocorrelation's window latency). Score per\-symbol, not per\-sample:

```cpp
class BitScorer {
public:
    void configure(double sampleRateHz, double onMs, double offMs, double guardFrac = 0.2);
    // call once per ADC sample; returns true and fills outCorrect when a symbol just completed
    bool update(bool detectorBit, bool truthBit, bool& outCorrect);
    uint64_t totalSymbols() const;
    uint64_t errorSymbols() const;
    double ber() const; // errorSymbols / totalSymbols
};
```

Within each symbol period, exclude a guard band (`guardFrac`, default 0.2 → the first and last 20% of the symbol) from both the majority vote and the ground truth reference — this covers detector latency and edge transitions without needing a hand\-tuned per\-detector delay compensation. Majority\-vote `detectorBit` over the remaining (middle 60%) samples of the symbol, compare to `truthBit` sampled the same way, and count a symbol error on mismatch. This is a correctness\-relevant design choice — do not skip the guard band, the three detectors have materially different latencies (Goertzel and autocorrelation both hold their value for a full block/window, biquad only has filter settling) and an ungapped comparison will penalize the slower detectors even when they're working correctly.

### 3\.8 `detect_sweep` — the actual deliverable {#38-detect_sweep-the-actual-deliverable}

The GUI (Phase 4) is for eyeballing behavior; this headless sweep is what actually answers "which detector wins," and it's cheap to build now that the detector classes exist. CLI, no GUI dependency:

```
./detect_sweep --sweep-param noise-mv --values 10,15,20,25,30,35,40,50,60,80 \
    --symbols-per-point 2000 --seed 1 --out sweep.csv
```

For each swept value: reconfigure `SignalChain` (and hold every other `sigsim::Params` at its default), run `symbols-per-point` OOK symbols through all three detectors in parallel (one `SignalChain::step()` call feeds all three `processSample()` calls each tick), score each with its own `BitScorer`, write one CSV row: `sweep_value,ber_biquad,ber_goertzel,ber_autocorr`. Support at minimum `--sweep-param noise-mv` and `--sweep-param fade-max` (the two parameters that most directly stress detection). Default sweep for noise\-mv: 10 to 80 in steps of 5 (matches the example above), 2000 symbols/point (2000 bits × 2ms/bit \= 4s of simulated time per point — enough for a BER estimate to be meaningful down to roughly 1e\-3 before running out of error events).

### 3\.9 Acceptance criteria for Phase 3 {#39-acceptance-criteria-for-phase-3}

Extend `tools/verify.py` (same script, don't create a second one) with:

- Biquad coefficient check: configure at a known `f0`/`Q`, verify the analytic magnitude response at `f0` is unity and at `f0 * 2` (or another off\-target ratio) matches the RBJ formula's predicted attenuation, independent of the noisy signal chain.
- Goertzel check: feed a pure known\-amplitude tone at `targetFreqHz` with zero noise, verify `rawMagnitude` converges to the expected value within a few percent; feed a tone at `targetFreqHz * 1.5` (well outside the bin), verify magnitude is suppressed by at least 10x relative to on\-target.
- Autocorrelation check: this is the one that would have caught the original integer\-lag bug. Feed a pure tone at `targetFreqHz` with zero noise, verify `rawMagnitude` (normalized correlation) exceeds 0.9 sustained over a full window; then verify that with the fractional\-lag interpolation disabled (test flag or a temporary rounded\-lag build) the same input scores measurably lower — this proves the fix matters, not just that the detector runs.
- `detect_sweep` sanity check: at `noise-mv=10` (easy case, well above the 20mV signal) all three detectors should score BER below 1%; at `noise-mv=200` (signal buried far below noise) all three should be well above 20% (near chance). Anything that doesn't show this monotonic\-ish degradation indicates a wiring bug, not a genuine result — worth asserting in the script rather than only eyeballing the sweep CSV.

## Phase 4 — GUI (ImGui \+ implot real\-time scope) {#phase-4-gui-imgui-implot-real-time-scope}

### 4\.1 Dependencies {#41-dependencies}

```
third_party/
  imgui/     (docking branch, vendored as source, not a submodule — this repo ships as a zip)
  implot/    (vendored as source)
```

GLFW3 \+ OpenGL3 backend (`imgui_impl_glfw`, `imgui_impl_opengl3`). Verify `glfw3` dev package and an OpenGL loader are available in the target build environment before starting — this cannot be smoke\-tested in a headless sandbox (no display), so Phase 4 correctness verification happens on Yens's machine, not in CI. Build the CLI targets (`sigsim_dump`, `detect_sweep`) independently of the GUI target so Phase 3's headless tooling keeps working even if the GUI dependencies aren't present in a given environment.

### 4\.2 File layout {#42-file-layout}

```
src/gui/
  main.cpp           GLFW+OpenGL3+ImGui+implot bootstrap, frame loop
  RingBuffer.h        fixed-capacity circular buffer for scope traces
  ScopeState.h        owns SignalChain + 3 detectors + ring buffers, advances by one sim step
```

### 4\.3 Frame loop {#43-frame-loop}

Wall\-clock\-driven, accumulator\-based, fixed simulation step (`1/sampleRateHz`), clamped accumulator (e.g. max 5 simulated steps' worth of catch\-up per frame) to avoid a spiral of death on window resize or a stalled frame. Run/Pause toggle gates whether `ScopeState::advance()` is called; the UI itself (sliders, plots) still redraws every frame regardless.

### 4\.4 Control panel (left column) {#44-control-panel-left-column}

Every `sigsim::Params` field and every `DetectorParams` field as a live slider/checkbox, grouped by section (ADC/sampling, noise, analog front end, carrier, fading, OOK modulation, detector tuning). Reset button restores all defaults. Run/Pause. A read\-out of current per\-detector BER (windowed, e.g. trailing 200 symbols — reuse `BitScorer` logic but with a rolling window instead of a whole\-run total) gives a live numeric complement to the visual scope.

### 4\.5 Scope panel (right, `ImPlot::BeginSubplots` grid) {#45-scope-panel-right-implotbeginsubplots-grid}

1. ADC input: `adcV` (quantized, noisy) vs. `idealV` (ground truth), same axes.
2. Biquad bandpass output \+ envelope.
3. Goertzel magnitude (step plot — it only updates once per block).
4. Autocorrelation magnitude.
5. Fade multiplier (ground truth), log\-scale y\-axis.
6. Bit decisions: ground\-truth `bitTruth` and each detector's `bitDecision`, stacked with a vertical offset per row so agreement/disagreement is visually obvious.

### 4\.6 Reconfiguration and ring buffers {#46-reconfiguration-and-ring-buffers}

When any `sigsim::Params` or `DetectorParams` slider changes, call `SignalChain::configure()` / `IDetector::configure()` again with the updated struct rather than reconstructing objects (this is why 3.2 requires `configure()` to be safely re\-callable). When the scope time\-window slider changes, resize the ring buffers accordingly (drop old content — no need to preserve history across a resize).

### 4\.7 Acceptance criteria for Phase 4 {#47-acceptance-criteria-for-phase-4}

No automated test — verify by running on Yens's machine: all six sub\-plots render and update in real time at the default parameters; each control\-panel slider visibly changes its corresponding sub\-plot within one frame of release; Run/Pause and Reset behave as expected; CPU usage stays reasonable at default settings (autocorrelation is the one to watch, per 3.6 — confirm the O(1)\-per\-sample sliding\-window implementation was actually used and not a naive O(W) recompute, since the frame loop calls it live at 200k samples/sec).

## Phase 5 — Build & packaging {#phase-5-build-packaging}

### 5\.1 CMake {#51-cmake}

Extend the existing `CMakeLists.txt` (don't replace it):

```cmake
add_library(sigsim_detect INTERFACE)   # header-only, like sigsim itself
target_include_directories(sigsim_detect INTERFACE include)

add_executable(detect_dump src/detect_dump.cpp)
target_link_libraries(detect_dump PRIVATE sigsim_detect)

add_executable(detect_sweep src/detect_sweep.cpp)
target_link_libraries(detect_sweep PRIVATE sigsim_detect)

find_package(glfw3 QUIET)
find_package(OpenGL QUIET)
if(glfw3_FOUND AND OpenGL_FOUND)
    add_executable(sigsim_gui
        src/gui/main.cpp
        third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_glfw.cpp
        third_party/imgui/backends/imgui_impl_opengl3.cpp
        third_party/implot/implot.cpp third_party/implot/implot_items.cpp
    )
    target_include_directories(sigsim_gui PRIVATE
        include third_party/imgui third_party/imgui/backends third_party/implot)
    target_link_libraries(sigsim_gui PRIVATE sigsim_detect glfw OpenGL::GL)
else()
    message(STATUS "glfw3/OpenGL not found — skipping sigsim_gui target (CLI tools still build)")
endif()
```

### 5\.2 Smoke test sequence (run in order, each gating the next) {#52-smoke-test-sequence-run-in-order-each-gating-the-next}

1. `cmake -S . -B build && cmake --build build -j4` — all targets that can build headlessly (`sigsim_dump`, `detect_dump`, `detect_sweep`) must succeed with zero warnings, same bar as Phase 2.
2. `python3 tools/verify.py` — all Phase 2 \+ Phase 3 checks (3.9) pass.
3. `./build/detect_sweep --sweep-param noise-mv --values 10,20,40,80 --symbols-per-point 500 --out /tmp/sweep_smoke.csv` runs to completion and produces the expected monotonic\-ish BER trend (3.9's sanity check).
4. On a machine with a display: build `sigsim_gui`, confirm it launches and Phase 4.7's checklist passes.

### 5\.3 Packaging {#53-packaging}

Same shape as the Phase 1–2 delivery: zip the whole tree (source, vendored `third_party/imgui` \+ `third_party/implot`, updated `CMakeLists.txt`, updated `README.md` covering the new targets and how to run `detect_sweep`). Update `README.md`'s "Not in this deliverable" section to remove the items this phase completes.

## Notes for whoever implements this {#notes-for-whoever-implements-this}

- Reuse `sigsim::AnalogFrontEnd`'s exact\-exponential one\-pole filter for the biquad detector's envelope follower (3.4) instead of writing a second version — that class was specifically corrected in Phase 2 after `verify.py` caught a 30% error in the naive `dt/(RC+dt)` approximation, and the same failure mode applies to any other one\-pole filter in this codebase.
- The autocorrelation fractional\-lag fix (3.6) is the one piece of this plan that corrects an actual bug in the original ChatGPT\-authored design, not just an implementation detail — don't silently revert to integer\-lag rounding for simplicity.
- `thresholdFrac` is intentionally a single shared value applied to each detector's own independently\-tracked peak (3.3, 3.5) — this is a deliberate design carried over from the original plan, not an oversight to "fix" into per\-detector thresholds. Per\-detector `peakTauMs`/envelope tuning is where the real per\-detector tuning happens.