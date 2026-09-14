# Detector test bench: investigation and fixes — 2026-09-14

## Summary

Started from a report that all three bit detectors (biquad, Goertzel,
autocorrelation) were reading ~50% BER (chance level) in the GUI. That
turned out to be mostly a parameter problem (noise cranked to near its max
slider value against a tiny carrier), but digging into it surfaced two real
bugs in the code, both now fixed and verified:

1. The GUI's "Sample rate Fs" slider never reached the detectors.
2. The autocorrelation detector could falsely declare "carrier present"
   during OOK silence, worst exactly when the signal was cleanest.

Also ran a quantitative comparison of the three detection algorithms using
the project's own `detect_sweep` tool.

## 1. GUI bug: Fs slider not synced to detectors

**Symptom:** moving "Sample rate Fs" in the GUI away from its 200 000 Hz
default pinned all three detectors at ~48-50% BER regardless of noise.

**Cause:** `ScopeState::configure()` (`src/gui/ScopeState.h`) copied the
`DetectorParams` struct as given, but nothing ever set
`dp.sampleRateHz = p.sampleRateHz`. `DetectorParams::sampleRateHz` has no
GUI slider of its own — it silently stayed at its struct default (200 000)
forever, while `p.sampleRateHz` (driven by the Fs slider) diverged from it.
All three detectors compute their filter coefficients / Goertzel bin /
autocorrelation lag directly from `DetectorParams::sampleRateHz`, so any Fs
change desynced every detector from the signal it was actually receiving.

**Fix:** one line in `ScopeState::configure()`:
```cpp
dp_.sampleRateHz = p_.sampleRateHz;
```
`dp_.targetFreqHz` was deliberately left alone — it has its own GUI slider,
used intentionally to test detectors against an off-target tone.

**Verification:** standalone before/after test at Fs=452 400 Hz, low noise,
fading off:

| | biquad BER | goertzel BER | autocorr BER |
|---|---|---|---|
| before fix | 47.7% | 47.7% | 0.0% |
| after fix | 0.0% | 2.3% | 0.0% |

Status: **fixed, committed** (`6d4cbd2`).

## 2. Autocorrelation detector bug: false "carrier present" during silence

**Symptom:** at low noise (most clearly at `noise-mv=0`), the
autocorrelation detector read ~50% BER — chance level — while biquad and
Goertzel read ~0%. Counter-intuitively, autocorrelation got *worse* as the
signal got cleaner.

**Investigation:** built a Python model that reimplements
`AutocorrDetector::processSample` sample-for-sample against a real ADC
dump (`detect_dump`, noise=0, fading off) to inspect every internal
quantity through an on→off transition. Initially suspected floating-point
cancellation in the O(1) sliding-window sums; raising the near-zero guard
from `1e-12` only partially helped (BER dropped from ~50% to ~47%, and
per-symbol wrong-fraction from ~70% to ~40%), which ruled that out as the
main cause.

Sweeping `autocorrWindowMs` (0.05 – 0.5 ms) showed the misclassified
duration scaling with window size but never reaching zero even at the
smallest window — inconsistent with "the window hasn't forgotten the
carrier yet," the mechanism the code's own comments assumed.

**Root cause:** the AC-coupling filter (`dcTrack_`, a single-pole low-pass
removing DC offset before correlation) has a settling time comparable to
several correlation-lag steps. Right after the carrier turns off, that
filter is still mid-transient — its slowly-decaying residual looks nearly
identical to itself shifted by the ~4-5 sample correlation lag. Since the
correlation output is a scale-invariant ratio, it reads that self-similar
decay as strong carrier correlation (confirmed numerically: the ratio hit
exactly `1.000000` for several consecutive samples after each pulse ended),
and stayed there for most of the off-gap because the ratio doesn't care
that the *absolute* signal power has collapsed to almost nothing.

**Fix** (`include/detect/AutocorrDetector.h`): added a second peak
tracker (`powerPeak_`) that tracks the recent peak of the raw signal power
(`sumXX_`). The correlation ratio is now only trusted when current power is
at least 5% of that tracked peak — real carrier energy stays close to peak
power, while the quasi-DC transient's power collapses almost immediately
after turn-off, so the gate rejects it. A secondary fix (minimum absolute
energy floor, raised from `1e-12` to `1e-9`) also guards the tail case
where the running sums are genuinely down to floating-point noise.

**Verification:**
- `noise-mv=0` sweep: autocorr BER **0.4994 → 0.0000**; biquad/Goertzel
  unchanged.
- Window-size sweep (0.05-0.5 ms), previously 40-90% of every off-gap
  misclassified at every size tested: **0% wrong at every size** after the
  fix.
- Full noise sweep (0-500 mV) and fade-depth sweep, plus 4 different RNG
  seeds at 40/100/200 mV noise: **identical to pre-fix results** at every
  non-zero-noise point — no regression to normal operation.

Status: **fixed, not yet committed** (working tree change in
`include/detect/AutocorrDetector.h`).

## 3. Algorithm comparison

Using `detect_sweep` (project defaults except the swept parameter, 5000
BitScorer symbols/point):

**Noise sweep** (seed 1):

| Noise RMS (mV) | biquad | goertzel | autocorr |
|---:|---:|---:|---:|
| 10-40 | 0% | 0.02% | 0% |
| 100 | 0.72% | **0.02%** | 0.76% |
| 200 | 2.46% | **0.24%** | 2.42% |
| 400 | 7.76% | **1.18%** | 9.2% |
| 500 | 10.3% | **1.82%** | 12.1% |

**Fade-depth sweep** (seed 1, noise fixed at default 40 mV):

| fadeMaxMult | biquad | goertzel | autocorr |
|---:|---:|---:|---:|
| 1 | 26.0% | **2.38%** | 31.1% |
| 5 | 3.44% | **0.08%** | 3.82% |
| 20 | 0.38% | **0.02%** | 0.5% |

Cross-checked with 3 additional RNG seeds at 40/100/200 mV noise — absolute
BER varies by seed (fading produces large amplitude swings, so some seeds
are just harder), but **the ranking was identical in every run**.

**Conclusion: Goertzel is the best detector**, consistently 5-10x lower BER
than biquad or autocorrelation at matched noise, and the most robust to
deep fades. Biquad and autocorrelation are close to each other, with
autocorrelation usually the weaker of the two now that its silent-period
bug is fixed.

## Build state

- Fresh clean build (`cmake -S . -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release`
  + `cmake --build`) of all four targets succeeds with no warnings from
  either change.
- `build/*.exe` (sigsim_gui, detect_dump, detect_sweep, sigsim_dump) are
  up to date with current source as of this report.
- The original `build/` CMakeCache still points at a stale path from
  before this repo was moved into Nextcloud (`C:\Users\Intern\Desktop\...`),
  so `cmake --build build` in place will still fail until that cache is
  regenerated — binaries were built in scratch directories and copied in
  instead. Not yet fixed.

## Outstanding

- `include/detect/AutocorrDetector.h` fix is in the working tree,
  uncommitted.
- `build/`'s stale CMakeCache (see above).
- `tests/` directory and `sweep.csv` at the repo root are untracked and
  were not created by this investigation — noted but not touched.
