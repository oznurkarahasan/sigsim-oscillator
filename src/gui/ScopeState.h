#pragma once
#include "sigsim/SignalChain.h"
#include "detect/BiquadDetector.h"
#include "detect/GoertzelDetector.h"
#include "detect/AutocorrDetector.h"
#include "detect/BitScorer.h"
#include "RingBuffer.h"

#include <algorithm>
#include <vector>

// Windowed (rolling) BER: same idea as BitScorer's whole-run total, but
// only over the last windowSymbols() completed symbols, so the GUI's
// live BER read-out tracks a slider change within a couple hundred
// symbols instead of being diluted by the entire session's history.
class RollingBer {
public:
    void configure(int windowSymbols) {
        capacity_ = std::max(1, windowSymbols);
        correctFlags_.assign(static_cast<size_t>(capacity_), true);
        pos_ = 0;
        count_ = 0;
        errors_ = 0;
    }

    void push(bool correct) {
        if (count_ == capacity_) {
            if (!correctFlags_[static_cast<size_t>(pos_)]) --errors_;
        } else {
            ++count_;
        }
        correctFlags_[static_cast<size_t>(pos_)] = correct;
        if (!correct) ++errors_;
        pos_ = (pos_ + 1) % capacity_;
    }

    double ber() const { return count_ > 0 ? static_cast<double>(errors_) / count_ : 0.0; }

private:
    int capacity_ = 200;
    int pos_ = 0;
    int count_ = 0;
    int errors_ = 0;
    std::vector<bool> correctFlags_;
};

// Owns SignalChain + all three detectors + their scorers + the scope's
// ring buffers, and advances everything by exactly one simulation step at
// a time. This is the only class the GUI frame loop and control panel
// touch -- main.cpp never reaches into sigsim/detect types directly.
class ScopeState {
public:
    void configure(const sigsim::Params& p, const detect::DetectorParams& dp, int windowSamples) {
        p_ = p;
        dp_ = dp;

        chain_.configure(p_);
        biquad_.configure(dp_);
        goertzel_.configure(dp_);
        autocorr_.configure(dp_);

        // Same latency compensation as detect_sweep -- see BitScorer.h.
        scoreBiquad_.configure(p_.sampleRateHz, p_.ookOnMs, p_.ookOffMs, 0.2, 0);
        scoreGoertzel_.configure(p_.sampleRateHz, p_.ookOnMs, p_.ookOffMs, 0.2, goertzel_.blockSamples());
        scoreAutocorr_.configure(p_.sampleRateHz, p_.ookOnMs, p_.ookOffMs, 0.2, autocorr_.windowSamples() / 2);

        rollBiquad_.configure(200);
        rollGoertzel_.configure(200);
        rollAutocorr_.configure(200);

        setWindowSamples(windowSamples);
    }

    // Advance exactly one ADC-rate sample and push it into every trace.
    void advance() {
        const sigsim::AdcSample s = chain_.step();
        const detect::DetectorOutput ob = biquad_.processSample(s.adcV);
        const detect::DetectorOutput og = goertzel_.processSample(s.adcV);
        const detect::DetectorOutput oa = autocorr_.processSample(s.adcV);

        bool correct;
        if (scoreBiquad_.update(ob.bitDecision, s.bitTruth, correct))   rollBiquad_.push(correct);
        if (scoreGoertzel_.update(og.bitDecision, s.bitTruth, correct)) rollGoertzel_.push(correct);
        if (scoreAutocorr_.update(oa.bitDecision, s.bitTruth, correct)) rollAutocorr_.push(correct);

        const float tMs = static_cast<float>(s.timeS * 1000.0);
        bufTime_.push(tMs);
        bufAdc_.push(static_cast<float>(s.adcV));
        bufIdeal_.push(static_cast<float>(s.idealV));
        bufBiquadBandpass_.push(static_cast<float>(biquad_.lastBandpassOutput()));
        bufBiquadEnvelope_.push(static_cast<float>(ob.rawMagnitude));
        bufGoertzelMag_.push(static_cast<float>(og.rawMagnitude));
        bufAutocorrMag_.push(static_cast<float>(oa.rawMagnitude));
        bufFadeMult_.push(static_cast<float>(s.fadeMult));
        bufBitTruth_.push(s.bitTruth ? 1.0f : 0.0f);
        bufBiquadBit_.push(ob.bitDecision ? 1.0f : 0.0f);
        bufGoertzelBit_.push(og.bitDecision ? 1.0f : 0.0f);
        bufAutocorrBit_.push(oa.bitDecision ? 1.0f : 0.0f);
    }

    // Re-applies the current p_/dp_ (call after any control-panel slider
    // edits, or for the Reset button) -- SignalChain::configure() and each
    // detector's configure() are safely re-callable at runtime (3.2), so
    // no object ever needs to be reconstructed. Clears the scope buffers
    // too: mixing pre- and post-change samples in the same trace would be
    // more confusing than a brief visual reset.
    void reconfigure() { configure(p_, dp_, windowSamples_); }

    void setWindowSamples(int windowSamples) {
        windowSamples_ = std::max(4, windowSamples);
        bufTime_.setCapacity(windowSamples_);
        bufAdc_.setCapacity(windowSamples_);
        bufIdeal_.setCapacity(windowSamples_);
        bufBiquadBandpass_.setCapacity(windowSamples_);
        bufBiquadEnvelope_.setCapacity(windowSamples_);
        bufGoertzelMag_.setCapacity(windowSamples_);
        bufAutocorrMag_.setCapacity(windowSamples_);
        bufFadeMult_.setCapacity(windowSamples_);
        bufBitTruth_.setCapacity(windowSamples_);
        bufBiquadBit_.setCapacity(windowSamples_);
        bufGoertzelBit_.setCapacity(windowSamples_);
        bufAutocorrBit_.setCapacity(windowSamples_);
    }

    sigsim::Params& params() { return p_; }
    detect::DetectorParams& detectorParams() { return dp_; }
    int windowSamples() const { return windowSamples_; }

    double berBiquad() const { return rollBiquad_.ber(); }
    double berGoertzel() const { return rollGoertzel_.ber(); }
    double berAutocorr() const { return rollAutocorr_.ber(); }

    const RingBuffer& bufTime() const { return bufTime_; }
    const RingBuffer& bufAdc() const { return bufAdc_; }
    const RingBuffer& bufIdeal() const { return bufIdeal_; }
    const RingBuffer& bufBiquadBandpass() const { return bufBiquadBandpass_; }
    const RingBuffer& bufBiquadEnvelope() const { return bufBiquadEnvelope_; }
    const RingBuffer& bufGoertzelMag() const { return bufGoertzelMag_; }
    const RingBuffer& bufAutocorrMag() const { return bufAutocorrMag_; }
    const RingBuffer& bufFadeMult() const { return bufFadeMult_; }
    const RingBuffer& bufBitTruth() const { return bufBitTruth_; }
    const RingBuffer& bufBiquadBit() const { return bufBiquadBit_; }
    const RingBuffer& bufGoertzelBit() const { return bufGoertzelBit_; }
    const RingBuffer& bufAutocorrBit() const { return bufAutocorrBit_; }

private:
    sigsim::Params p_;
    detect::DetectorParams dp_;
    int windowSamples_ = 4000;

    sigsim::SignalChain chain_;
    detect::BiquadDetector biquad_;
    detect::GoertzelDetector goertzel_;
    detect::AutocorrDetector autocorr_;

    detect::BitScorer scoreBiquad_, scoreGoertzel_, scoreAutocorr_;
    RollingBer rollBiquad_, rollGoertzel_, rollAutocorr_;

    RingBuffer bufTime_, bufAdc_, bufIdeal_;
    RingBuffer bufBiquadBandpass_, bufBiquadEnvelope_;
    RingBuffer bufGoertzelMag_, bufAutocorrMag_;
    RingBuffer bufFadeMult_;
    RingBuffer bufBitTruth_, bufBiquadBit_, bufGoertzelBit_, bufAutocorrBit_;
};
