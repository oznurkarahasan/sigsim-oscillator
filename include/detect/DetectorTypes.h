#pragma once

// Common types shared by every detector implementation. Zero dependency
// on sigsim/* -- a detector only ever consumes a stream of doubles
// (AdcSample::adcV), never the sigsim types directly.

namespace detect {

// One detector's output for a single input sample.
struct DetectorOutput {
    double rawMagnitude    = 0.0;   // detector-native units: volts (biquad),
                                     // power^0.5/N (Goertzel), correlation
                                     // coefficient (autocorr)
    double normalizedLevel = 0.0;   // rawMagnitude / tracked peak -- dimensionless,
                                     // comparable across detectors
    bool   bitDecision     = false; // normalizedLevel > thresholdFrac
};

struct DetectorParams {
    double sampleRateHz  = 200000.0; // must match sigsim::Params::sampleRateHz
    double targetFreqHz  = 47400.0;  // nominal carrier -- must match sigsim::Params::signalFreqHz
    double thresholdFrac = 0.5;      // shared fractional threshold, applied to each
                                      // detector's OWN peak -- deliberate, not per-detector
    double peakTauMs     = 20.0;     // peak-tracker decay time constant

    // detector-specific, ignored by the other two:
    double biquadQ          = 8.0;
    double biquadEnvTauMs   = 0.3;   // envelope-follower smoothing after rectification
    double goertzelBlockMs  = 1.0;   // block length -- tied to the OOK symbol length (default 1ms)

    // Deliberately NOT tied 1:1 to the OOK symbol length the way
    // goertzelBlockMs is (that was the development plan's original
    // default, matching the on/off period). A sliding correlation window
    // the same length as the off-gap can never fully flush the previous
    // on-pulse's carrier before the next on-pulse arrives -- verified
    // empirically: with windowMs=1.0 (== the default 1ms off-gap),
    // rawMagnitude stays elevated for virtually the entire off period
    // (this is the window legitimately still containing real carrier
    // samples, not a bug), and BER pins at 0.5 (chance) regardless of
    // noise level. A quarter of the symbol length gives the window three
    // quarters of the off-gap to fully forget the carrier before the next
    // decision is scored.
    double autocorrWindowMs = 0.25;
};

} // namespace detect
