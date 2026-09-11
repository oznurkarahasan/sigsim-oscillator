#pragma once
#include <cmath>

namespace sigsim {

// Single-pole RC low-pass, run at the OVERSAMPLED tick rate (not at Fs).
//
// This matters: the RC filter is a real analog stage in front of the ADC.
// If you filtered at Fs, noise generated at Fs is already band-limited to
// Fs/2 by construction and no aliasing could ever show up, no matter how
// high you set the cutoff -- which would make the "anti-alias filter
// cutoff" control a no-op. Running the filter at the oversampled rate and
// decimating afterwards (see SignalChain) means energy the filter didn't
// remove between Fs/2 and the oversampled Nyquist genuinely folds back
// into the working band, same as it would on a real board.
class AnalogFrontEnd {
public:
    void configure(double cutoffHz, double tickRateHz) {
        const double rc = 1.0 / (2.0 * M_PI * cutoffHz);
        const double dt = 1.0 / tickRateHz;
        // Exact zero-order-hold discretization of dy/dt = (x-y)/RC, i.e. the
        // true sampled response of an RC circuit driven by a piecewise-constant
        // input over each tick -- correct for any dt/RC ratio, not just dt<<RC.
        // (The simpler dt/(RC+dt) backward-Euler form warps badly once the
        // cutoff gets within an order of magnitude of the tick rate, which
        // matters here because cutoff is user-tunable up toward Fs.)
        alpha_ = 1.0 - std::exp(-dt / rc);
    }

    double next(double x) {
        y_ += alpha_ * (x - y_);
        return y_;
    }

    void reset(double y0 = 0.0) { y_ = y0; }

private:
    double alpha_ = 1.0;
    double y_     = 0.0;
};

} // namespace sigsim
