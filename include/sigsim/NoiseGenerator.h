#pragma once
#include <random>

namespace sigsim {

// White Gaussian noise, generated at whatever tick rate it's called at.
// Called at the oversampled rate so it is white up to the oversampled
// Nyquist, not just up to Fs/2 -- that's what lets the RC filter's
// rolloff (or lack of it) actually matter, and lets energy above Fs/2
// alias back into band after decimation, same as real hardware.
class NoiseGenerator {
public:
    void configure(double rmsVolts, uint32_t seed) {
        rmsVolts_ = rmsVolts;
        rng_.seed(seed);
    }

    double next() { return dist_(rng_) * rmsVolts_; }

private:
    double rmsVolts_ = 0.0;
    std::mt19937 rng_;
    std::normal_distribution<double> dist_{0.0, 1.0};
};

} // namespace sigsim
