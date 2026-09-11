#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sigsim {

// Clips to [0, Vref] then rounds to the nearest of 2^bits codes.
class AdcQuantizer {
public:
    void configure(int bits, double vref) {
        bits_    = bits;
        vref_    = vref;
        maxCode_ = (1u << bits_) - 1u;
    }

    // Returns the raw code; writes the code converted back to volts into outV.
    uint32_t quantize(double v, double& outV) const {
        const double clamped = std::clamp(v, 0.0, vref_);
        const double frac    = clamped / vref_;
        const auto   code    = static_cast<uint32_t>(std::lround(frac * maxCode_));
        outV = (static_cast<double>(code) / static_cast<double>(maxCode_)) * vref_;
        return code;
    }

    uint32_t maxCode() const { return maxCode_; }

private:
    int      bits_    = 12;
    double   vref_    = 3.3;
    uint32_t maxCode_ = 4095;
};

} // namespace sigsim
