#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace detect {

// Per-symbol majority-vote scoring against ground truth.
//
// Per-sample bitDecision is noisy at symbol edges and lags behind ground
// truth by each detector's own latency (biquad envelope settling,
// Goertzel's block latency, autocorrelation's window latency), so scoring
// happens per-symbol, not per-sample.
//
// A "symbol" here is one constant-ground-truth interval of the OOK gate:
// one on-pulse (length onMs, truth=1) or one off-gap (length offMs,
// truth=0) -- these alternate. That is the only definition under which a
// symbol has a single well-defined ground-truth value throughout, which is
// what the guard-band majority vote below requires. (A "symbol" that spans
// a full on+off cycle would straddle the truth transition in its own
// interior, giving no well-defined ground truth to score against with the
// default 50/50 on/off split -- so it is not used here.)
//
// truthDelaySamples compensates each detector's OWN known decision
// latency before the guard band ever runs: Goertzel and autocorrelation
// are configured (by default) with block/window lengths equal to a full
// symbol, so their decision for symbol k isn't just "a little late", it
// reflects the PREVIOUS symbol's data almost entirely -- e.g. Goertzel's
// block-hold value is revealed only at the boundary of the next symbol
// after the block it was computed from, a full symbol (not a fraction of
// one) late, and deterministically so. A 20% guard trim (meant to cover
// small edge jitter) cannot absorb a whole-symbol systematic offset; empi-
// rically, without this compensation Goertzel/autocorrelation score near
// chance-level BER independent of noise, which is a wiring bug, not a
// physical result. truthDelaySamples shifts the ground-truth reference
// back by each detector's own known, computed-not-tuned latency (an exact
// block length for Goertzel, half the window for autocorrelation's
// continuously-updated sliding sum) so the guard band is left to do only
// what it was designed for: residual jitter and edge transitions.
class BitScorer {
public:
    void configure(double sampleRateHz, double onMs, double offMs,
                    double guardFrac = 0.2, int truthDelaySamples = 0) {
        onSamples_  = std::max(1, static_cast<int>(sampleRateHz * onMs / 1000.0 + 0.5));
        offSamples_ = std::max(1, static_cast<int>(sampleRateHz * offMs / 1000.0 + 0.5));
        guardFrac_  = guardFrac;
        truthDelay_ = std::max(0, truthDelaySamples);
        truthRing_.assign(static_cast<size_t>(std::max(1, truthDelay_)), true);
        reset();
    }

    void reset() {
        onPhase_       = true;
        symbolLen_     = onSamples_;
        posInSymbol_   = 0;
        detectorVotesTotal_ = 0;
        detectorVotesTrue_  = 0;
        truthVotesTotal_    = 0;
        truthVotesTrue_     = 0;
        totalSymbols_  = 0;
        errorSymbols_  = 0;
        std::fill(truthRing_.begin(), truthRing_.end(), true);
        truthRingPos_ = 0;
    }

    // Call once per ADC sample. Returns true and fills outCorrect when a
    // symbol just completed on this call.
    bool update(bool detectorBit, bool truthBitNow, bool& outCorrect) {
        outCorrect = false;

        bool truthBit = truthBitNow;
        if (truthDelay_ > 0) {
            truthBit = truthRing_[truthRingPos_];
            truthRing_[truthRingPos_] = truthBitNow;
            truthRingPos_ = (truthRingPos_ + 1) % truthRing_.size();
        }

        const double guardLo = guardFrac_ * symbolLen_;
        const double guardHi = (1.0 - guardFrac_) * symbolLen_;
        if (posInSymbol_ >= guardLo && posInSymbol_ < guardHi) {
            ++detectorVotesTotal_;
            if (detectorBit) ++detectorVotesTrue_;
            ++truthVotesTotal_;
            if (truthBit) ++truthVotesTrue_;
        }
        ++posInSymbol_;

        if (posInSymbol_ < symbolLen_) return false;

        const bool detectorMajority = detectorVotesTotal_ > 0 &&
            (detectorVotesTrue_ * 2 > detectorVotesTotal_);
        const bool truthMajority = truthVotesTotal_ > 0 &&
            (truthVotesTrue_ * 2 > truthVotesTotal_);

        outCorrect = (detectorMajority == truthMajority);
        ++totalSymbols_;
        if (!outCorrect) ++errorSymbols_;

        onPhase_     = !onPhase_;
        symbolLen_   = onPhase_ ? onSamples_ : offSamples_;
        posInSymbol_ = 0;
        detectorVotesTotal_ = detectorVotesTrue_ = 0;
        truthVotesTotal_    = truthVotesTrue_    = 0;
        return true;
    }

    uint64_t totalSymbols() const { return totalSymbols_; }
    uint64_t errorSymbols() const { return errorSymbols_; }
    double ber() const {
        return totalSymbols_ > 0 ? static_cast<double>(errorSymbols_) / static_cast<double>(totalSymbols_) : 0.0;
    }

private:
    int onSamples_  = 200;
    int offSamples_ = 200;
    double guardFrac_ = 0.2;

    bool onPhase_     = true;
    int  symbolLen_   = 200;
    int  posInSymbol_ = 0;

    int detectorVotesTotal_ = 0;
    int detectorVotesTrue_  = 0;
    int truthVotesTotal_    = 0;
    int truthVotesTrue_     = 0;

    uint64_t totalSymbols_ = 0;
    uint64_t errorSymbols_ = 0;

    int truthDelay_ = 0;
    std::vector<bool> truthRing_{true};
    size_t truthRingPos_ = 0;
};

} // namespace detect
