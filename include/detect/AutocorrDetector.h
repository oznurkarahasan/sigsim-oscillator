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

        windowLen_ = std::max(2, static_cast<int>(std::lround(p_.sampleRateHz * p_.autocorrWindowMs / 1000.0)));

        // Ring buffer needs to look back at most windowLen_ + lagHi_ samples.
        capacity_ = static_cast<size_t>(windowLen_ + lagHi_ + 4);
        xbuf_.assign(capacity_, 0.0);

        // Cutoff well below the carrier so this tracks only the slow DC/
        // offset component, not the signal itself.
        dcTrack_.configure(p_.targetFreqHz / 20.0, p_.sampleRateHz);

        peak_.configure(p_.peakTauMs, p_.sampleRateHz);
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

        const double normLo = std::sqrt(std::max(sumXX_, 0.0) * std::max(eLo_, 0.0));
        const double normHi = std::sqrt(std::max(sumXX_, 0.0) * std::max(eHi_, 0.0));
        const double rNormLo = normLo > 1e-12 ? rLo_ / normLo : 0.0;
        const double rNormHi = normHi > 1e-12 ? rHi_ / normHi : 0.0;

        double rawMagnitude;
        if (fractionalLagEnabled_) {
            rawMagnitude = (1.0 - frac_) * rNormLo + frac_ * rNormHi;
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
};

} // namespace detect
