#pragma once
#include <algorithm>
#include <cmath>

namespace detect {

// Shared AGC / peak-follower used by all three detectors. Converts a
// detector's raw, unit-specific magnitude into a normalized 0..1-ish level
// that a single shared thresholdFrac can be applied to, and lets all three
// track the fading amplitude envelope instead of comparing against a fixed
// voltage.
//
// peakTauMs default 20ms: long enough to not track individual noise spikes
// (bit period is 1ms), short enough to follow the fade envelope's 50ms
// keyframe rate with some lag.
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

} // namespace detect
