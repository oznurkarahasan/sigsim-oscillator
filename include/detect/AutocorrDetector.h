#pragma once
#include "DetectorTypes.h"
#include "IDetector.h"
#include "PeakTracker.h"
#include "sigsim/AnalogFrontEnd.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace detect {

// Autocorrelation detector -- fractional lag (this is the fix, not the
// original integer-lag design).
//
// sampleRateHz / targetFreqHz is not an integer (200000/47400 = 4.219).
// An integer-lag autocorrelation at round(fs/f0) = 4 actually resonates at
// fs/4 = 50kHz, 5.5% off target -- over a 1ms bit (~47 carrier cycles) this
// phase-slips enough to significantly degrade the correlation peak. This
// uses two-tap linear interpolation between the adjacent integer lags
// instead of rounding.
//
// r(L) and its normalization are maintained with O(1)-per-sample sliding-
// window updates (add the new term, subtract the term leaving the window)
// rather than resumming the whole window every sample -- this is the most
// compute-heavy of the three detectors either way, and a naive O(W)
// recompute per sample is exactly what must be avoided (it matters live at
// 200k samples/sec in the Phase 4 GUI).
//
// AC-coupling: AdcSample::adcV carries a large DC offset (default 1.6V)
// next to a tiny carrier (default 20mV). Correlating the raw ADC voltage
// against itself makes that offset dominate every lag equally -- rawMagni-
// tude reads near 1.0 whether or not the carrier is actually present,
// because x[n]*x[n-L] is overwhelmingly offset^2 regardless of L. (Biquad
// doesn't need this: its own bandpass response already rejects DC by
// construction.) Subtracting a slow low-pass estimate of the input before
// it ever reaches the correlator removes the offset while leaving the
// carrier essentially untouched (the low-pass cutoff is set far below the
// carrier, so it tracks only the offset).
class AutocorrDetector : public IDetector {
public:
    void configure(const DetectorParams& p) override {
        p_ = p;

        const double lagF = p_.sampleRateHz / p_.targetFreqHz;
        lagLo_ = static_cast<int>(std::floor(lagF));
        lagHi_ = lagLo_ + 1;
        frac_  = lagF - lagLo_;

        const double omega = 2.0 * M_PI * p_.targetFreqHz / p_.sampleRateHz;
        cosOmega_ = std::cos(omega);
        sinOmega_ = std::sin(omega);

        windowLen_ = std::max(2, static_cast<int>(std::lround(p_.sampleRateHz * p_.autocorrWindowMs / 1000.0)));

        // Ring buffer needs to look back at most windowLen_ + lagHi_ samples.
        capacity_ = static_cast<size_t>(windowLen_ + lagHi_ + 4);
        xbuf_.assign(capacity_, 0.0);

        // Cutoff well below the carrier so this tracks only the slow DC/
        // offset component, not the signal itself.
        dcTrack_.configure(p_.targetFreqHz / 20.0, p_.sampleRateHz);

        peak_.configure(p_.peakTauMs, p_.sampleRateHz);
        // Tracks the RECENT PEAK of sumXX_ itself (signal power, volts^2),
        // same AGC timescale as peak_. Used below to tell "real but
        // vanishingly small energy" apart from "no real energy at all" in
        // a scale-invariant way -- see the comment at its use site for why
        // this is needed on top of the plain min-energy floor.
        powerPeak_.configure(p_.peakTauMs, p_.sampleRateHz);
        reset();
    }

    void reset() override {
        std::fill(xbuf_.begin(), xbuf_.end(), 0.0);
        idx_ = 0;
        sumXX_ = 0.0;
        rLo_ = rHi_ = 0.0;
        eLo_ = eHi_ = 0.0;
        dcTrack_.reset(0.0);
        peak_.reset();
        powerPeak_.reset();
    }

    // Test-only hook (Phase 3 acceptance criteria, 3.9): forces the nearest
    // integer lag instead of fractional-lag interpolation, so verify.py can
    // demonstrate the fix matters rather than only that the detector runs.
    void setFractionalLagEnabled(bool enabled) { fractionalLagEnabled_ = enabled; }

    DetectorOutput processSample(double xRaw) override {
        const double x = xRaw - dcTrack_.next(xRaw); // AC-couple, see class comment

        const long idx = idx_;
        xbuf_[static_cast<size_t>(idx) % capacity_] = x;

        sumXX_ += x * x;
        if (idx >= windowLen_) {
            const double leaving = xAt(idx - windowLen_);
            sumXX_ -= leaving * leaving;
        }

        updateLag(idx, lagLo_, rLo_, eLo_, x);
        updateLag(idx, lagHi_, rHi_, eHi_, x);

        ++idx_;

        // Bug fix (part 1 of 2): a min-energy floor alone isn't enough --
        // see part 2 below for why. This still helps for the last stretch
        // where sumXX_/eLo_/eHi_ really have decayed into pure floating-
        // point cancellation residue (~1e-17..1e-8) rather than real
        // signal, and the old `> 1e-12` guard on the *combined* norm let
        // those through (normLo_ was routinely ~1e-7..1e-8 there, above
        // it) even though the underlying energies are numerical noise.
        constexpr double kMinEnergyV2 = 1e-9;
        const bool hasEnergyLo = sumXX_ > kMinEnergyV2 && eLo_ > kMinEnergyV2;
        const bool hasEnergyHi = sumXX_ > kMinEnergyV2 && eHi_ > kMinEnergyV2;
        const double normLo = std::sqrt(std::max(sumXX_, 0.0) * std::max(eLo_, 0.0));
        const double normHi = std::sqrt(std::max(sumXX_, 0.0) * std::max(eHi_, 0.0));

        // Bug fix (part 2 of 2), the actual root cause: right after the
        // carrier stops, dcTrack_ (the AC-coupling low-pass) hasn't fully
        // settled to the new DC level yet -- its own transient decay is
        // slow relative to the ~4-5 sample lag used for correlation (its
        // cutoff is targetFreqHz/20, i.e. an RC time constant several
        // times longer than one lag step). Over a handful of samples, a
        // slowly-decaying near-DC residual looks nearly IDENTICAL to
        // itself shifted by that short lag -- x[n] =~ x[n-lag] -- which
        // drives r(lag) up toward sqrt(sumXX_*e(lag)), i.e. a correlation
        // ratio near the maximum of 1.0. This is real signal (not
        // floating-point noise, confirmed by its smooth exponential
        // decay), so a min-energy floor can't distinguish it -- by the
        // time its *absolute* energy is small enough to floor out, it has
        // already read as "carrier present" for most of the off-gap
        // (reproduced empirically: shrinking autocorrWindowMs shortened
        // but never eliminated this, because it isn't a window-forgetting
        // problem at all).
        //
        // Fix: the correlation RATIO is scale-invariant by design (that's
        // the whole point of normalizing by norm), so it cannot by itself
        // tell "a full-amplitude carrier" from "a tiny near-DC transient
        // that happens to correlate perfectly with itself". What's
        // missing is an ABSOLUTE check: is there currently as much signal
        // POWER as when the carrier was actually last on? powerPeak_
        // tracks the recent peak of sumXX_ itself (same AGC timescale as
        // peak_) and update() returns sumXX_ as a fraction of that peak.
        // Once the carrier turns off, sumXX_ collapses to a small
        // fraction of the tracked peak power almost immediately (unlike
        // the ratio, which stays pinned near 1.0) -- gating on that
        // fraction rejects the quasi-DC lock-in while still passing
        // genuine weak-but-real carrier energy (which sits close to peak
        // power, not a small fraction of it).
        constexpr double kMinPowerFrac = 0.05;
        const double powerFrac = powerPeak_.update(std::max(sumXX_, 0.0));
        const bool hasPower = powerFrac > kMinPowerFrac;

        const bool validLo = hasEnergyLo && hasPower;
        const bool validHi = hasEnergyHi && hasPower;
        const double rNormLo = validLo ? rLo_ / normLo : 0.0;
        const double rNormHi = validHi ? rHi_ / normHi : 0.0;

        double rawMagnitude;
        if (fractionalLagEnabled_) {
            // Two-point amplitude/phase reconstruction, not a linear blend
            // (see class comment): for a near-monochromatic signal, r(L) is
            // ~cosine-shaped in L, r(L) = rho*cos(omega*(L-Ltrue)). Linearly
            // blending rNormLo and rNormHi is a chord under that concave
            // arc and UNDERESTIMATES the true peak rho -- empirically, at
            // this project's own default 47.4kHz/200kHz (frac=0.219), it
            // actually scores measurably WORSE than plain nearest-integer
            // rounding, the opposite of what the fix is supposed to do.
            // Since omega is known exactly (from targetFreqHz), rho can be
            // solved for exactly instead: writing rNormHi in terms of
            // rNormLo and the unknown phase (rHi = rho*cos(theta+omega) =
            // rNormLo*cos(omega) - rho*sin(theta)*sin(omega)) gives
            // rho*sin(theta) directly, and rho = sqrt(rNormLo^2 + (rho*sin(theta))^2).
            const double crossTerm = (rNormLo * cosOmega_ - rNormHi) / sinOmega_;
            rawMagnitude = std::sqrt(rNormLo * rNormLo + crossTerm * crossTerm);
        } else {
            // Nearest-integer-lag fallback, for the "does the fix matter" check.
            rawMagnitude = (frac_ < 0.5) ? rNormLo : rNormHi;
        }

        DetectorOutput out;
        out.rawMagnitude    = rawMagnitude;
        out.normalizedLevel = peak_.update(std::fabs(rawMagnitude));
        out.bitDecision     = out.normalizedLevel > p_.thresholdFrac;
        return out;
    }

    const char* name() const override { return "autocorr"; }

    // Approximate decision latency: unlike Goertzel this is a continuously-
    // updated sliding window, not a hard block-hold, so there's no single
    // exact delay -- the correlation at time t is a uniformly-weighted sum
    // over the last windowSamples() samples, so its effective time
    // reference is roughly the CENTER of that window, half a window back.
    // Exposed so BitScorer can compensate for it (see BitScorer.h's
    // truthDelaySamples).
    int windowSamples() const { return windowLen_; }

private:
    // r(L)/e(L) sliding-window update: entering term for the newest sample,
    // leaving term for the sample that just fell out of the W-length window
    // (only once the window has filled). Samples requested before time 0
    // read as 0 from the zero-initialized ring buffer, which is exactly the
    // correct "signal is zero before start" convention for both r and e.
    void updateLag(long idx, int lag, double& r, double& e, double xNew) {
        const double delayed = xAt(idx - lag);
        r += xNew * delayed;
        e += delayed * delayed;
        if (idx >= windowLen_) {
            const double leavingNew     = xAt(idx - windowLen_);
            const double leavingDelayed = xAt(idx - windowLen_ - lag);
            r -= leavingNew * leavingDelayed;
            e -= leavingDelayed * leavingDelayed;
        }
    }

    double xAt(long j) const {
        if (j < 0) return 0.0;
        return xbuf_[static_cast<size_t>(j) % capacity_];
    }

    DetectorParams p_;
    int    lagLo_ = 4;
    int    lagHi_ = 5;
    double frac_  = 0.0;
    double cosOmega_ = 1.0;
    double sinOmega_ = 0.0;
    int    windowLen_ = 200;
    bool   fractionalLagEnabled_ = true;

    std::vector<double> xbuf_;
    size_t capacity_ = 1;
    long   idx_ = 0;
    sigsim::AnalogFrontEnd dcTrack_;

    double sumXX_ = 0.0;
    double rLo_ = 0.0, rHi_ = 0.0;
    double eLo_ = 0.0, eHi_ = 0.0;

    PeakTracker peak_;
    PeakTracker powerPeak_;
};

} // namespace detect
