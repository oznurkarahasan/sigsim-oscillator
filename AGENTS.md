# AGENTS.md — 47 kHz Detector Test Bench

Rules for any agent (or human) working on this repo. Read this before touching code. If something here conflicts with a ticket/prompt you were given, this file wins on *how* to build; the prompt wins on *what* to build.

## What this project is

A test bench for detecting a faint (~20mV), not-exactly-known-frequency (~47.4kHz) OOK-modulated tone in a noisy (~40mV), offset (~1.6V), amplitude-fading ADC signal, comparing three detection methods (biquad bandpass, Goertzel, autocorrelation). Phases 1–2 (signal-chain simulation) are done and verified. See `README.md` for architecture and `docs/development-plan-phase-3-5.md` (or wherever the phase plan lives) for what's still open.

Two audiences read this repo: whoever runs it on a PC to compare detectors, and — eventually — whoever ports the winning detector to an embedded target (ESP32/Pico-class MCU). Keep that second audience in mind: code that's needlessly hard to port later is a cost paid twice.

## Non-negotiables

**Never approximate a discretization when an exact one is cheap.** This codebase already had one real bug from this: the RC filter used `alpha = dt/(RC+dt)` (backward-Euler), which is only accurate when `dt << RC` — and broke by ~30% RMS in exactly the parameter range the defaults use. It was replaced with the exact zero-order-hold form, `alpha = 1 - exp(-dt/RC)`, which is correct for any `dt/RC` ratio at the same cost. Any new one-pole filter (envelope followers, AGC decay, anything called "lowpass" or "smoothing") uses this exact form, or `sigsim::AnalogFrontEnd`/the shared peak-tracker directly rather than a third copy of the same filter. Don't add a second bespoke one-pole implementation anywhere in the tree — reuse the existing class.

**Never trust a number you haven't independently verified.** `tools/verify.py` exists because "it builds and looks right on the scope" already produced a wrong RC filter, a Nyquist-edge measurement artifact, and a settling-transient bug in this session alone — all three caught only because each claim was checked against an independent calculation (an analytic formula, a from-scratch Python simulation, or a controlled A/B run), not by inspection. Every new piece of signal math (a filter, a detector, a scoring method) gets a corresponding check added to `tools/verify.py` before it's considered done — same file, not a second test script. A check that can't fail (an assertion that's trivially true for any implementation, correct or not) doesn't count; if you can't think of how the check could catch a real bug, it isn't testing anything.

**Diagnose before fixing.** When a check fails, find out *why* — is it the implementation, or the test's own methodology? This session hit both kinds (a genuine filter bug, and separately a test measuring gain at exactly the Nyquist frequency where phase-dependent readout makes any measurement meaningless). Fixing the wrong one silences a real bug or breaks a passing test. Don't touch code until you can state which one it is and why, ideally with a small standalone calculation (see the independent-Python-simulation pattern used to confirm the AR(1) prediction in this session) rather than a guess.

**State assumptions and units explicitly, in the code, not just in your head.** Every tunable in `Params`/`DetectorParams` names its unit in a comment (mV vs V, ms vs s, Hz) — this codebase has already had a near-miss from an RMS/peak mixup and a mV/V scaling slip. If you add a parameter, follow the existing comment style in `Types.h`.

## Architecture rules

- **Header-only, one class per file, one responsibility per class.** `include/sigsim/*.h` and (from Phase 3) `include/detect/*.h` follow this — `Oscillator` only oscillates, `AdcQuantizer` only quantizes. Don't fold two stages into one class because it's convenient; the point of this layout is that each stage is independently testable.
- **`SignalChain::step()` (and each `IDetector::processSample()`) is the only interface the rest of the project depends on.** GUI code, CLI harnesses, and sweep tools all consume these same entry points — don't reach into a stage's internals from outside its own class.
- **No GUI dependency in anything that computes.** Signal generation, filtering, detection, and scoring must build and run headlessly (`sigsim_dump`, `detect_dump`, `detect_sweep`) with zero ImGui/implot/GLFW dependency. The GUI is a consumer of these, never the other way around. This is what let Phases 1–3 be fully verified in a sandbox with no display.
- **Ground truth travels with the data, not alongside it.** `AdcSample` carries `idealV`/`bitTruth`/`fadeMult` next to the actual ADC reading specifically so scoring code never has to re-derive or separately track truth. Any new per-sample struct that needs scoring follows the same pattern.
- **Config structs are re-configurable, not just constructible.** `configure()` must be safely callable again on a live object with new parameters (the GUI reconfigures on every slider release). Don't write a class that only sets up correctly from its constructor.

## Build & verify — run these, in this order, before calling anything done

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4          # zero warnings, not just zero errors
python3 tools/verify.py          # every check must PASS, not just "mostly pass"
```

If you added a detector or sweep tool, also run it against at least one known-easy case (high SNR) and one known-hard case (SNR near zero) and confirm the result moves the direction it should — a flat or backwards BER-vs-noise curve means something is wired wrong even if every individual unit check passes.

Before considering a zip/package done, extract it to a clean directory and build from *that* — this session caught nothing extra doing it, but it's the only way to catch a file that works locally because of a stale build artifact but isn't actually in the package.

## Style

- C++17, `-Wall -Wextra` clean.
- Prefer `std::` facilities already used in the codebase (`std::clamp`, `std::normal_distribution`, etc.) over hand-rolled equivalents.
- Comments explain *why* a formula or constant is what it is (see `AnalogFrontEnd.h`, `AutocorrDetector` once written) — not what the next line of code obviously does.
- No magic numbers without a name and a unit. `1.0e-12` as an epsilon in a divide-by-peak guard gets a named constant or at least an inline comment on why that value.

## When you're not sure

Make the reasonable, testable assumption, note it in the commit message or PR description, and add a `tools/verify.py` check that would catch you being wrong — don't block on asking. Do ask (or leave a clearly marked `TODO`/open question in the relevant plan doc) when the ambiguity is about what the detectors should optimize for (e.g. "prioritize the embedded-deployable candidate over the best PC-only result") rather than how to implement something — that's Yens's call, not an implementation detail.
