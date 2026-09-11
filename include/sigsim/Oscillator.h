#pragma once
#include <cmath>

namespace sigsim {

// Phase-accumulator sine generator. Deliberately not sin(2*pi*f*t): with a
// running phase accumulator, freqHz can be changed live without a phase
// discontinuity (a live "frequency drift" slider will not click/pop).
class Oscillator {
public:
    void configure(double freqHz, double tickRateHz) {
        tickRateHz_ = tickRateHz;
        setFrequency(freqHz);
    }

    // Safe to call every tick if you want the carrier frequency to drift.
    void setFrequency(double freqHz) {
        freqHz_ = freqHz;
        phaseInc_ = 2.0 * M_PI * freqHz_ / tickRateHz_;
    }

    double next() {
        const double s = std::sin(phase_);
        phase_ += phaseInc_;
        if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
        if (phase_ < 0.0) phase_ += 2.0 * M_PI;
        return s;
    }

    void reset(double phase = 0.0) { phase_ = phase; }

private:
    double tickRateHz_ = 1.0;
    double freqHz_     = 0.0;
    double phaseInc_   = 0.0;
    double phase_      = 0.0;
};

} // namespace sigsim
