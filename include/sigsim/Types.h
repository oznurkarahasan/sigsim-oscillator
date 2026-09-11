#pragma once
#include <cstdint>

// All tunable parameters and the per-sample output record.
// Kept as plain structs so this header has zero dependency on anything
// else in the project (GUI, detectors, etc. can include just this).

namespace sigsim {

struct Params {
    // --- ADC / sampling ---
    double sampleRateHz      = 200000.0; // Fs: ADC output rate
    int    oversample        = 16;       // internal "analog domain" tick multiplier
    int    adcBits            = 12;
    double vrefV              = 3.3;

    // --- noise ---
    double noiseRmsMv         = 40.0;    // white noise, RMS, at the ADC pin

    // --- analog front end ---
    double rcCutoffHz         = 100000.0; // single-pole RC low-pass before the ADC

    // --- carrier ---
    double signalFreqHz       = 47400.0; // not exactly 47 kHz, by design
    double signalAmplitudeMv  = 20.0;    // nominal (unfaded) zero-to-peak amplitude
    double offsetV            = 1.6;     // DC offset, not exactly Vref/2

    // --- amplitude fading (slow random walk, source moving near/far) ---
    bool   fadeEnabled        = true;
    double fadeUpdateMs       = 50.0;    // random-walk keyframe interval
    double fadeStepStd        = 0.08;    // random-walk step size, normalized 0..1 space
    double fadeMinMult        = 0.05;    // min amplitude multiplier
    double fadeMaxMult        = 120.0;   // max amplitude multiplier -> drives hard clipping

    // --- 500 baud on/off test modulation ---
    bool   ookEnabled         = true;
    double ookOnMs            = 1.0;
    double ookOffMs           = 1.0;

    uint32_t rngSeed          = 12345;
};

// One ADC-rate output sample, carrying both what a real ADC would read
// and the ground truth needed to score detectors later (bit error rate,
// fade tracking, etc).
struct AdcSample {
    double   timeS     = 0.0;
    double   idealV    = 0.0;   // pre-noise, pre-filter carrier + offset (ground truth)
    double   preAdcV   = 0.0;   // post-noise, post-RC-filter, pre-quantization
    uint32_t code       = 0;     // raw ADC code, 0..(2^bits - 1)
    double   adcV       = 0.0;   // quantized code converted back to volts
    bool     bitTruth   = false; // ground-truth OOK bit (true = carrier commanded on)
    double   fadeMult   = 1.0;   // ground-truth amplitude multiplier
};

} // namespace sigsim
