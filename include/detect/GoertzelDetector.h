#pragma once
#include "DetectorTypes.h"
#include "IDetector.h"
#include "PeakTracker.h"
#include "sigsim/AnalogFrontEnd.h"

#include <algorithm>
#include <cmath>

namespace detect {

// Block-based single-bin power, block length tied to the OOK symbol
// duration so one block ~= one bit decision.
//
//   N     = round(sampleRateHz * goertzelBlockMs / 1000)
//   omega = 2*pi*targetFreqHz / sampleRateHz
//   coeff = 2*cos(omega)   -- NOT tied to an integer bin: Goertzel works for
//                             any target frequency at a given block length,
//                             which is what makes it robust to the carrier
//                             not being exactly 47kHz.
//
// processSample() accumulates into the current block and only updates
// rawMagnitude (and therefore bitDecision) once every N samples -- the
// previous value is held in between. This means up to goertzelBlockMs of
// decision latency; BitScorer's truthDelaySamples compensates for exactly
// this (see BitScorer.h and blockSamples() below).
//
// AC-coupling: AdcSample::adcV carries a large DC offset (default 1.6V)
// next to a tiny carrier (default 20mV). At the default 200kHz/1ms block
// (200 samples), targetFreqHz doesn't land on an exact DFT bin of the
// block (47400Hz over 200 samples at 200kHz Fs is ~47.4 cycles, not an
// integer), so the block has real spectral leakage from DC into the target
// bin -- large enough, given how much bigger the offset is than the
// carrier, to make "on" and "off" blocks read similar raw magnitudes.
// Subtracting a slow low-pass estimate of the input before accumulation
// removes the offset while leaving the carrier essentially untouched.
class GoertzelDetector : public IDetector {
public:
    void configure(const DetectorParams& p) override {
        p_ = p;
        blockN_ = std::max(1, static_cast<int>(std::lround(p_.sampleRateHz * p_.goertzelBlockMs / 1000.0)));
        const double omega = 2.0 * M_PI * p_.targetFreqHz / p_.sampleRateHz;
        coeff_ = 2.0 * std::cos(omega);
        dcTrack_.configure(p_.targetFreqHz / 20.0, p_.sampleRateHz);
        // PeakTracker's decay_ is calibrated for update() being called once
        // per sample at sampleRateHz -- but Goertzel only calls it once per
        // BLOCK (every blockN_ samples), since rawMagnitude only changes
        // that often. Configuring it at the raw sampleRateHz would make the
        // configured peakTauMs blockN_ times too slow in wall-clock terms
        // (e.g. a 20ms tau silently becoming ~4s with the default 200-
        // sample block): a single inflated startup block would then take
        // seconds, not milliseconds, to decay out of the peak, corrupting
        // every bitDecision in between. Configure against the actual call
        // rate instead.
        peak_.configure(p_.peakTauMs, p_.sampleRateHz / blockN_);
        reset();
    }

    void reset() override {
        s1_ = s2_ = 0.0;
        countInBlock_ = 0;
        held_.rawMagnitude = 0.0;
        held_.normalizedLevel = 0.0;
        held_.bitDecision = false;
        dcTrack_.reset(0.0);
        peak_.reset();
    }

    DetectorOutput processSample(double adcVoltsRaw) override {
        const double adcVolts = adcVoltsRaw - dcTrack_.next(adcVoltsRaw); // AC-couple, see class comment
        const double s0 = adcVolts + coeff_ * s1_ - s2_;
        s2_ = s1_;
        s1_ = s0;
        ++countInBlock_;

        if (countInBlock_ >= blockN_) {
            const double power = s1_ * s1_ + s2_ * s2_ - coeff_ * s1_ * s2_;
            const double rawMagnitude = std::sqrt(std::max(power, 0.0)) / static_cast<double>(blockN_);

            held_.rawMagnitude    = rawMagnitude;
            held_.normalizedLevel = peak_.update(rawMagnitude);
            held_.bitDecision     = held_.normalizedLevel > p_.thresholdFrac;

            s1_ = s2_ = 0.0;
            countInBlock_ = 0;
        }

        return held_;
    }

    const char* name() const override { return "goertzel"; }

    // Exact, deterministic decision latency: the block a decision reflects
    // is only revealed at the end of the NEXT block, so the held value at
    // any sample is always exactly blockSamples() behind the data it
    // represents. Exposed so BitScorer can compensate for it (see
    // BitScorer.h's truthDelaySamples).
    int blockSamples() const { return blockN_; }

private:
    DetectorParams p_;
    int    blockN_ = 200;
    double coeff_  = 0.0;

    double s1_ = 0.0, s2_ = 0.0;
    int    countInBlock_ = 0;

    DetectorOutput held_;
    sigsim::AnalogFrontEnd dcTrack_;
    PeakTracker peak_;
};

} // namespace detect
