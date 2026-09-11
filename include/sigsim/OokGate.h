#pragma once

namespace sigsim {

// 500-baud-style on/off test modulation: onMs of carrier, offMs of silence,
// repeating. Gates the carrier only -- noise is present continuously,
// same as a real receiver front end.
class OokGate {
public:
    void configure(double onMs, double offMs, bool enabled) {
        onS_     = onMs / 1000.0;
        offS_    = offMs / 1000.0;
        periodS_ = onS_ + offS_;
        enabled_ = enabled;
        t_ = 0.0;
    }

    // Advance by dt seconds, return true if the carrier should be on.
    bool step(double dt) {
        if (!enabled_) return true;
        t_ += dt;
        if (periodS_ > 0.0) {
            while (t_ >= periodS_) t_ -= periodS_;
        }
        return t_ < onS_;
    }

    void reset() { t_ = 0.0; }

private:
    double onS_     = 0.001;
    double offS_    = 0.001;
    double periodS_ = 0.002;
    double t_       = 0.0;
    bool   enabled_ = true;
};

} // namespace sigsim
