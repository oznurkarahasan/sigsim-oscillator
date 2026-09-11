#pragma once
#include <random>
#include <cmath>
#include <algorithm>

namespace sigsim {

// Slow random-walk amplitude multiplier, modelling a signal source that
// drifts near/far over time (including hitting hard clipping when close).
//
// The walk happens in a normalized [0,1] space at a slow "keyframe" rate
// (fadeUpdateMs), and is linearly interpolated between keyframes so the
// value fed to the carrier every tick is continuous, not a staircase.
// The normalized value is then mapped geometrically (log-linear) onto
// [fadeMinMult, fadeMaxMult], because amplitude falling off with distance
// is a multiplicative process, not an additive one.
class FadeEnvelope {
public:
    void configure(double updateMs, double stepStd, double minMult, double maxMult, uint32_t seed) {
        updateS_ = updateMs / 1000.0;
        stepStd_ = stepStd;
        minMult_ = minMult;
        maxMult_ = maxMult;
        rng_.seed(seed);
        current_ = 0.5;
        elapsed_ = 0.0;
        target_ = drawNext();
    }

    // Advance by dt seconds, return the current amplitude multiplier.
    double step(double dt) {
        elapsed_ += dt;
        while (updateS_ > 0.0 && elapsed_ >= updateS_) {
            elapsed_ -= updateS_;
            current_ = target_;
            target_ = drawNext();
        }
        const double frac = updateS_ > 0.0 ? std::clamp(elapsed_ / updateS_, 0.0, 1.0) : 1.0;
        const double norm = current_ + (target_ - current_) * frac;
        return toMultiplier(norm);
    }

private:
    double drawNext() {
        double v = current_ + normal_(rng_) * stepStd_;
        // Reflect at the [0,1] boundaries instead of clamping, so the walk
        // keeps moving (a hard clamp would let it get stuck at 0 or 1).
        if (v < 0.0) v = -v;
        if (v > 1.0) v = 2.0 - v;
        return std::clamp(v, 0.0, 1.0);
    }

    double toMultiplier(double norm) const {
        const double logMin = std::log(minMult_);
        const double logMax = std::log(maxMult_);
        return std::exp(logMin + norm * (logMax - logMin));
    }

    double updateS_ = 0.05;
    double stepStd_ = 0.08;
    double minMult_ = 0.05;
    double maxMult_ = 120.0;

    double current_ = 0.5; // normalized 0..1
    double target_  = 0.5; // normalized 0..1
    double elapsed_ = 0.0;

    std::mt19937 rng_;
    std::normal_distribution<double> normal_{0.0, 1.0};
};

} // namespace sigsim
