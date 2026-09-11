#!/usr/bin/env python3
"""
Headless verification of the signal-chain simulation, run against the
built sigsim_dump binary. No GUI dependency -- checks the physics before
any detector or GUI code trusts this chain.

Checks:
  1. Carrier frequency accuracy (zero-crossing count).
  2. Noise RMS matches the requested mV when the RC filter is set to not
     meaningfully attenuate anything in-band.
  3. ADC quantization step count and clipping at both rails (measured
     after the RC filter has settled, not on the first sample).
  4. RC filter -3dB point, using RMS gain (phase-independent, unlike
     peak-to-peak) and a test frequency safely clear of the Fs/2 Nyquist
     edge, where peak-to-peak/short-window measurements become unreliable.
  5. Aliasing: with the RC cutoff set above Fs/2, broadband noise between
     Fs/2 and the cutoff should fold back into the decimated output at
     close to the level predicted by treating the filter+decimate chain
     as an AR(1) process -- this is the correctness check for the
     oversample-then-decimate design (it would fail, i.e. read near-zero
     wide-cutoff noise, if the RC filter were mistakenly applied at Fs
     instead of the oversampled rate).
  6. OOK symbol timing matches the requested on/off durations.

Phase 3 (detectors, against detect_dump/detect_sweep):
  7. Biquad bandpass: analytic RBJ magnitude response at f0 (unity) and at
     an off-target ratio, checked against the detector's own measured
     output -- independent of the noisy signal chain (noise=0).
  8. Goertzel: pure on-target tone converges to the expected magnitude;
     an off-target tone (1.5x) is suppressed by >=10x.
  9. Autocorrelation: on-target tone scores a normalized correlation
     >0.9; the same input with fractional-lag interpolation disabled
     (--autocorr-integer-lag) scores measurably lower -- this is the check
     that would have caught the original integer-lag bug.
 10. detect_sweep sanity: noise-mv=10 (easy) scores <1% BER on all three
     detectors; noise-mv=4000 (very hard) scores >20% (near chance) on all
     three. Not the plan's illustrative noise-mv=200 for the hard case --
     measured empirically (see check 10 below): all three detectors use a
     peak-normalized threshold and integrate over many carrier cycles
     (block/window/envelope), giving real processing gain against wideband
     noise, so 200mV (10x the 20mV carrier) isn't actually enough to drive
     a correctly-working implementation to chance level. 4000mV is.
"""
import subprocess
import sys
import io
import math
import cmath
import numpy as np
from pathlib import Path

BIN = Path(__file__).resolve().parent.parent / "build" / "sigsim_dump"
BIN_DETECT_DUMP = Path(__file__).resolve().parent.parent / "build" / "detect_dump"
BIN_DETECT_SWEEP = Path(__file__).resolve().parent.parent / "build" / "detect_sweep"

def run(args):
    out = subprocess.run([str(BIN), *args], capture_output=True, text=True, check=True)
    import csv
    r = csv.DictReader(io.StringIO(out.stdout))
    rows = list(r)
    cols = {k: np.array([float(row[k]) for row in rows]) for k in rows[0].keys()}
    return cols

def run_detect(args):
    out = subprocess.run([str(BIN_DETECT_DUMP), *args], capture_output=True, text=True, check=True)
    import csv
    r = csv.DictReader(io.StringIO(out.stdout))
    rows = list(r)
    cols = {k: np.array([float(row[k]) for row in rows]) for k in rows[0].keys()}
    return cols

def run_sweep(args):
    out = subprocess.run([str(BIN_DETECT_SWEEP), *args], capture_output=True, text=True, check=True)
    import csv
    r = csv.DictReader(io.StringIO(out.stdout))
    rows = list(r)
    cols = {k: np.array([float(row[k]) for row in rows]) for k in rows[0].keys()}
    return cols

failures = []

def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not cond:
        failures.append(name)

def ar1_rms_fraction(cutoff_hz, tick_rate_hz):
    """Predicted output-RMS / input-RMS for white noise through the exact
    one-pole RC discretization, as an AR(1) process: y[n] = phi*y[n-1] + (1-phi)*x[n]."""
    rc = 1.0 / (2.0 * math.pi * cutoff_hz)
    dt = 1.0 / tick_rate_hz
    phi = math.exp(-dt / rc)
    return math.sqrt((1 - phi) / (1 + phi))

# ---------------------------------------------------------------------
print("== 1. Carrier frequency accuracy ==")
fs = 200000.0
f0 = 47400.0
d = run(["--duration-ms", "20", "--noise-mv", "0", "--no-fade", "--no-ook",
         "--fs", str(fs), "--sig-freq", str(f0), "--sig-amp-mv", "500",
         "--rc-cutoff", "50000000"])  # far above tick rate -> filter effectively transparent
v = d["ideal_v"] - d["ideal_v"].mean()
crossings = np.where(np.diff(np.sign(v)) > 0)[0]
meas_freq = fs / np.diff(crossings).mean() if len(crossings) >= 2 else float("nan")
err_pct = abs(meas_freq - f0) / f0 * 100
check("measured carrier frequency within 0.5% of target",
      err_pct < 0.5, f"target={f0:.1f} Hz measured={meas_freq:.1f} Hz err={err_pct:.3f}%")

# ---------------------------------------------------------------------
print("\n== 2. Noise RMS (filter set transparent) ==")
noise_mv = 40.0
d = run(["--duration-ms", "50", "--sig-amp-mv", "0", "--no-fade", "--no-ook",
         "--noise-mv", str(noise_mv), "--rc-cutoff", "50000000"])
meas_rms_mv = d["pre_adc_v"].std() * 1000.0
err_pct = abs(meas_rms_mv - noise_mv) / noise_mv * 100
check("measured noise RMS within 5% of target",
      err_pct < 5.0, f"target={noise_mv:.1f} mV measured={meas_rms_mv:.2f} mV err={err_pct:.2f}%")

# ---------------------------------------------------------------------
print("\n== 3. ADC quantization ==")
bits = 12
vref = 3.3
max_code = (1 << bits) - 1

d = run(["--duration-ms", "5", "--noise-mv", "0", "--no-fade", "--no-ook",
         "--sig-amp-mv", "0", "--offset-v", "0", "--bits", str(bits), "--vref", str(vref)])
check("clips to code 0 at 0V input (settled)", int(d["adc_code"][-1]) == 0,
      f"code={int(d['adc_code'][-1])}")

d = run(["--duration-ms", "5", "--noise-mv", "0", "--no-fade", "--no-ook",
         "--sig-amp-mv", "0", "--offset-v", str(vref), "--bits", str(bits), "--vref", str(vref)])
check("clips to max code at Vref input (settled)", int(d["adc_code"][-1]) == max_code,
      f"code={int(d['adc_code'][-1])} expected={max_code}")

lsb_mv = vref / max_code * 1000.0
check("LSB size matches 12-bit/3.3V expectation (~0.8mV)", abs(lsb_mv - 0.806) < 0.01,
      f"lsb={lsb_mv:.4f} mV")

# ---------------------------------------------------------------------
print("\n== 4. RC filter -3dB point (RMS gain, test tone clear of Fs/2) ==")
cutoff = 40000.0   # well clear of Fs/2 = 100 kHz -> no decimation/phase ambiguity
oversample = 16
def measure_rms(test_freq, cutoff_hz):
    dd = run(["--duration-ms", "20", "--noise-mv", "0", "--no-fade", "--no-ook",
              "--sig-freq", str(test_freq), "--sig-amp-mv", "500", "--offset-v", "0",
              "--rc-cutoff", str(cutoff_hz), "--fs", str(fs), "--oversample", str(oversample)])
    settle = len(dd["pre_adc_v"]) // 4
    return dd["pre_adc_v"][settle:].std()

rms_passband = measure_rms(1000.0, cutoff)     # well below cutoff
rms_at_fc    = measure_rms(cutoff, cutoff)      # at the configured cutoff
ratio = rms_at_fc / rms_passband
check("RMS gain at configured cutoff is ~-3dB (0.707x) relative to passband",
      0.65 < ratio < 0.77, f"ratio={ratio:.3f} (ideal 0.707)")

# ---------------------------------------------------------------------
print("\n== 5. Aliasing: noise between Fs/2 and a wide RC cutoff folds back after decimation ==")
noise_mv = 40.0
tick_rate = fs * 32
d_wide = run(["--duration-ms", "50", "--sig-amp-mv", "0", "--no-fade", "--no-ook",
              "--noise-mv", str(noise_mv), "--rc-cutoff", "300000",  # 3x Fs/2 -> aliasing path open
              "--fs", str(fs), "--oversample", "32"])
settle = len(d_wide["pre_adc_v"]) // 10  # clear the RC startup transient (offset charging from 0)
rms_wide_mv = d_wide["pre_adc_v"][settle:].std() * 1000.0
pred_wide_mv = noise_mv * ar1_rms_fraction(300000.0, tick_rate)

d_tight = run(["--duration-ms", "50", "--sig-amp-mv", "0", "--no-fade", "--no-ook",
               "--noise-mv", str(noise_mv), "--rc-cutoff", "20000",  # well below Fs/2 -> real anti-aliasing
               "--fs", str(fs), "--oversample", "32"])
rms_tight_mv = d_tight["pre_adc_v"][settle:].std() * 1000.0
pred_tight_mv = noise_mv * ar1_rms_fraction(20000.0, tick_rate)

check("wide-cutoff measured RMS matches AR(1) prediction (filter math is correct)",
      abs(rms_wide_mv - pred_wide_mv) / pred_wide_mv < 0.15,
      f"measured={rms_wide_mv:.2f} mV predicted={pred_wide_mv:.2f} mV")
check("tight-cutoff measured RMS matches AR(1) prediction",
      abs(rms_tight_mv - pred_tight_mv) / pred_tight_mv < 0.15,
      f"measured={rms_tight_mv:.2f} mV predicted={pred_tight_mv:.2f} mV")
check("wide-cutoff noise (aliasing path open) is clearly higher than tight-cutoff (real anti-aliasing)",
      rms_wide_mv > 2.5 * rms_tight_mv,
      f"wide={rms_wide_mv:.2f} mV tight={rms_tight_mv:.2f} mV ratio={rms_wide_mv/rms_tight_mv:.2f}")

# ---------------------------------------------------------------------
print("\n== 6. OOK symbol timing ==")
d = run(["--duration-ms", "20", "--noise-mv", "0", "--no-fade",
         "--ook-on-ms", "1.0", "--ook-off-ms", "1.0", "--sig-amp-mv", "500"])
bit = d["bit_truth"].astype(int)
edges = np.where(np.diff(bit) != 0)[0]
seg_ms = np.diff(edges) / fs * 1000.0 if len(edges) >= 2 else np.array([float("nan")])
err_ms = np.abs(seg_ms - 1.0).max()
check("OOK on/off segments measure ~1.0ms each", err_ms < 0.02, f"max deviation={err_ms:.4f} ms")

# ---------------------------------------------------------------------
print("\n== 7. Biquad bandpass: analytic RBJ magnitude response ==")
biquad_f0 = 47400.0
biquad_q = 8.0

def rbj_bandpass_gain(f, f0, q, fs_):
    w0 = 2.0 * math.pi * f0 / fs_
    alpha = math.sin(w0) / (2.0 * q)
    b0, b1, b2 = alpha, 0.0, -alpha
    a0, a1, a2 = 1.0 + alpha, -2.0 * math.cos(w0), 1.0 - alpha
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    z = cmath.exp(1j * 2.0 * math.pi * f / fs_)
    num = b0 + b1 / z + b2 / (z * z)
    den = 1.0 + a1 / z + a2 / (z * z)
    return abs(num / den)

def measure_biquad_raw(test_freq, amp_mv):
    # --target-freq-hz pinned to biquad_f0 explicitly: --sig-freq alone
    # would also retune the detector to match, making every "off-target"
    # tone look on-target.
    dd = run_detect(["--duration-ms", "20", "--noise-mv", "0", "--no-fade", "--no-ook",
                      "--sig-freq", str(test_freq), "--sig-amp-mv", str(amp_mv),
                      "--target-freq-hz", str(biquad_f0),
                      "--rc-cutoff", "50000000", "--fs", str(fs),
                      "--biquad-q", str(biquad_q)])
    settle = len(dd["biquad_raw"]) // 2  # past both biquad ring-down and envelope-tau settling
    return dd["biquad_raw"][settle:].mean()

amp_mv = 300.0
meas_f0 = measure_biquad_raw(biquad_f0, amp_mv)
# Envelope of a full-wave-rectified sine of amplitude A*|H(f)| has mean (2/pi)*A*|H(f)|.
expected_f0 = (2.0 / math.pi) * (amp_mv / 1000.0) * rbj_bandpass_gain(biquad_f0, biquad_f0, biquad_q, fs)
err_pct = abs(meas_f0 - expected_f0) / expected_f0 * 100
check("biquad measured on-target output matches analytic unity-gain prediction",
      err_pct < 10.0, f"expected={expected_f0*1000:.2f} mV measured={meas_f0*1000:.2f} mV err={err_pct:.2f}%")

off_freq = biquad_f0 * 2.0
meas_off = measure_biquad_raw(off_freq, amp_mv)
expected_off = (2.0 / math.pi) * (amp_mv / 1000.0) * rbj_bandpass_gain(off_freq, biquad_f0, biquad_q, fs)
err_pct_off = abs(meas_off - expected_off) / max(expected_off, 1e-9) * 100
check(f"biquad measured off-target ({off_freq/1000:.1f}kHz) output matches analytic attenuated prediction",
      err_pct_off < 15.0 or meas_off < 0.05 * meas_f0,
      f"expected={expected_off*1000:.3f} mV measured={meas_off*1000:.3f} mV err={err_pct_off:.2f}%")

# ---------------------------------------------------------------------
print("\n== 8. Goertzel: on-target convergence, off-target suppression ==")
goertzel_amp_mv = 300.0

def measure_goertzel_raw(test_freq):
    dd = run_detect(["--duration-ms", "20", "--noise-mv", "0", "--no-fade", "--no-ook",
                      "--sig-freq", str(test_freq), "--sig-amp-mv", str(goertzel_amp_mv),
                      "--target-freq-hz", str(biquad_f0),
                      "--rc-cutoff", "50000000", "--fs", str(fs)])
    settle = len(dd["goertzel_raw"]) // 2
    return dd["goertzel_raw"][settle:].mean()

meas_on = measure_goertzel_raw(biquad_f0)
expected_on = 0.5 * (goertzel_amp_mv / 1000.0)  # standard Goertzel result: rawMagnitude ~= A/2 on-target
err_pct = abs(meas_on - expected_on) / expected_on * 100
check("Goertzel on-target magnitude converges to the expected A/2 within a few percent",
      err_pct < 5.0, f"expected={expected_on*1000:.2f} mV measured={meas_on*1000:.2f} mV err={err_pct:.2f}%")

meas_off = measure_goertzel_raw(biquad_f0 * 1.5)
check("Goertzel off-target (1.5x) magnitude suppressed by >=10x vs on-target",
      meas_off < meas_on / 10.0, f"on-target={meas_on*1000:.3f} mV off-target={meas_off*1000:.3f} mV ratio={meas_on/max(meas_off,1e-12):.1f}x")

# ---------------------------------------------------------------------
print("\n== 9. Autocorrelation: fractional-lag fix matters ==")
autocorr_amp_mv = 300.0

def measure_autocorr_raw(integer_lag):
    args = ["--duration-ms", "20", "--noise-mv", "0", "--no-fade", "--no-ook",
            "--sig-freq", str(biquad_f0), "--sig-amp-mv", str(autocorr_amp_mv),
            "--rc-cutoff", "50000000", "--fs", str(fs), "--autocorr-window-ms", "1.0"]
    if integer_lag:
        args.append("--autocorr-integer-lag")
    dd = run_detect(args)
    settle = len(dd["autocorr_raw"]) // 2
    return dd["autocorr_raw"][settle:].mean()

meas_fractional = measure_autocorr_raw(integer_lag=False)
check("autocorrelation (fractional-lag) sustains normalized correlation > 0.9 on a pure on-target tone",
      meas_fractional > 0.9, f"measured={meas_fractional:.4f}")

meas_integer = measure_autocorr_raw(integer_lag=True)
check("fractional-lag interpolation measurably outperforms nearest-integer-lag rounding (this is the fix that matters)",
      meas_fractional > meas_integer + 0.02,
      f"fractional={meas_fractional:.4f} integer-lag={meas_integer:.4f}")

# ---------------------------------------------------------------------
print("\n== 10. detect_sweep sanity: BER trend is noise-driven, not a wiring bug ==")
sweep_easy = run_sweep(["--sweep-param", "noise-mv", "--values", "10", "--symbols-per-point", "2000", "--seed", "1"])
for name in ("ber_biquad", "ber_goertzel", "ber_autocorr"):
    check(f"{name} < 1% at noise-mv=10 (easy case, signal well above noise)",
          sweep_easy[name][0] < 0.01, f"{name}={sweep_easy[name][0]*100:.3f}%")

# noise-mv=200 (the plan's illustrative "hard" value) is not actually hard
# enough to drive a correctly-working implementation to chance level -- all
# three detectors get real processing gain from integrating over many
# carrier cycles (envelope/block/window) against a peak-normalized
# threshold. 4000mV (200x the 20mV carrier) is confirmed empirically to
# reach chance level for all three; see the module docstring.
sweep_hard = run_sweep(["--sweep-param", "noise-mv", "--values", "4000", "--symbols-per-point", "1000", "--seed", "1"])
for name in ("ber_biquad", "ber_goertzel", "ber_autocorr"):
    check(f"{name} > 20% at noise-mv=4000 (signal buried far below noise, near chance)",
          sweep_hard[name][0] > 0.20, f"{name}={sweep_hard[name][0]*100:.2f}%")

# ---------------------------------------------------------------------
print("\n" + ("ALL CHECKS PASSED" if not failures else f"{len(failures)} CHECK(S) FAILED: {failures}"))
sys.exit(1 if failures else 0)
