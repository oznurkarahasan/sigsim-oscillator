#pragma once
#include "DetectorTypes.h"
#include "IDetector.h"
#include "PeakTracker.h"
#include "sigsim/AnalogFrontEnd.h"

#include <cmath>

namespace detect {

// RBJ cookbook constant-0dB-peak-gain bandpass, rectifier, envelope
// follower, peak tracker.
//
//   w0    = 2*pi*targetFreqHz / sampleRateHz
//   alpha = sin(w0) / (2*Q)
//   b0 =  alpha, b1 = 0, b2 = -alpha
//   a0 = 1 + alpha, a1 = -2*cos(w0), a2 = 1 - alpha
//
// Per sample: y = biquad(x); rectified = |y|; envelope = onePoleLowpass(
// rectified, tau = biquadEnvTauMs). The envelope follower reuses
// sigsim::AnalogFrontEnd's exact zero-order-hold discretization rather than
// a second one-pole implementation -- that class was specifically corrected
// after tools/verify.py caught a ~30% RMS error in the naive dt/(RC+dt)
// approximation, and the same failure mode would apply here.
//
// biquadEnvTauMs default 0.3ms: fast enough to resolve the 1ms bit period
// (~3 time constants per bit, ~95% settled), slow enough to smooth the
// rectified carrier ripple (carrier period ~21us at 47.4kHz, so tau=0.3ms
// averages over ~14 carrier cycles).
class BiquadDetector : public IDetector {
public:
    void configure(const DetectorParams& p) override {
        p_ = p;
        const double w0    = 2.0 * M_PI * p_.targetFreqHz / p_.sampleRateHz;
        const double alpha = std::sin(w0) / (2.0 * p_.biquadQ);

        const double b0 =  alpha;
        const double b1 =  0.0;
        const double b2 = -alpha;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * std::cos(w0);
        const double a2 =  1.0 - alpha;

        // Normalize by a0 (standard Direct Form I).
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;

        envelope_.configure(envTauToCutoffHz(p_.biquadEnvTauMs), p_.sampleRateHz);
        peak_.configure(p_.peakTauMs, p_.sampleRateHz);
        reset();
    }

    void reset() override {
        x1_ = x2_ = 0.0;
        y1_ = y2_ = 0.0;
        envelope_.reset(0.0);
        peak_.reset();
    }

    DetectorOutput processSample(double adcVolts) override {
        const double y = b0_ * adcVolts + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = adcVolts;
        y2_ = y1_; y1_ = y;

        const double rectified = std::fabs(y);
        const double env = envelope_.next(rectified);

        DetectorOutput out;
        out.rawMagnitude    = env;
        out.normalizedLevel = peak_.update(env);
        out.bitDecision     = out.normalizedLevel > p_.thresholdFrac;
        return out;
    }

    const char* name() const override { return "biquad"; }

private:
    // sigsim::AnalogFrontEnd::configure() takes a cutoff frequency, not a
    // time constant directly; convert tau -> the equivalent one-pole cutoff
    // (tau = RC = 1/(2*pi*fc)).
    static double envTauToCutoffHz(double tauMs) {
        const double tauS = tauMs / 1000.0;
        return 1.0 / (2.0 * M_PI * tauS);
    }

    DetectorParams p_;
    double b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;

    sigsim::AnalogFrontEnd envelope_;
    PeakTracker peak_;
};

} // namespace detect
