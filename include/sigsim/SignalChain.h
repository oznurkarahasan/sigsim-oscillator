#pragma once
#include "Types.h"
#include "Oscillator.h"
#include "NoiseGenerator.h"
#include "FadeEnvelope.h"
#include "OokGate.h"
#include "AnalogFrontEnd.h"
#include "AdcQuantizer.h"

namespace sigsim {

// Ties the stages together in signal-flow order:
//
//   oscillator --x amplitude(fade, OOK)--> + offset --> + noise
//     --> RC low-pass (oversampled) --> decimate to Fs --> clip+quantize (ADC)
//
// step() is the only entry point the rest of the project (detectors, GUI,
// CLI harness) needs: it advances `oversample` internal ticks and returns
// exactly one ADC-rate sample, carrying both the quantized ADC reading and
// the ground truth (ideal voltage, true bit, true fade level) needed to
// score detectors later.
class SignalChain {
public:
    void configure(const Params& p) {
        p_ = p;
        const double tickRateHz = p_.sampleRateHz * p_.oversample;
        dtOver_ = 1.0 / tickRateHz;

        osc_.configure(p_.signalFreqHz, tickRateHz);
        noise_.configure(p_.noiseRmsMv / 1000.0, p_.rngSeed);
        fade_.configure(p_.fadeUpdateMs, p_.fadeStepStd, p_.fadeMinMult, p_.fadeMaxMult, p_.rngSeed + 1);
        gate_.configure(p_.ookOnMs, p_.ookOffMs, p_.ookEnabled);
        front_.configure(p_.rcCutoffHz, tickRateHz);
        adc_.configure(p_.adcBits, p_.vrefV);

        timeS_ = 0.0;
    }

    AdcSample step() {
        double idealV  = 0.0;
        double preAdcV = 0.0;
        bool   bitOn   = false;
        double fadeMult = 1.0;

        for (int i = 0; i < p_.oversample; ++i) {
            fadeMult = p_.fadeEnabled ? fade_.step(dtOver_) : 1.0;
            bitOn = gate_.step(dtOver_);

            const double amplitudeV = (p_.signalAmplitudeMv / 1000.0) * fadeMult * (bitOn ? 1.0 : 0.0);
            const double carrier = osc_.next() * amplitudeV;

            idealV = p_.offsetV + carrier;                 // ground truth: no noise, no filter
            const double withNoise = idealV + noise_.next();
            preAdcV = front_.next(withNoise);               // RC filter runs every oversampled tick

            timeS_ += dtOver_;
            // Every tick's filtered value is computed, but only the last one
            // in this block is kept below -- that's the decimation step, and
            // deliberately has no extra filtering attached to it.
        }

        AdcSample s;
        s.timeS    = timeS_;
        s.idealV   = idealV;
        s.preAdcV  = preAdcV;
        s.code     = adc_.quantize(preAdcV, s.adcV);
        s.bitTruth = bitOn;
        s.fadeMult = fadeMult;
        return s;
    }

    const Params& params() const { return p_; }

private:
    Params p_;
    double dtOver_ = 0.0;
    double timeS_  = 0.0;

    Oscillator      osc_;
    NoiseGenerator  noise_;
    FadeEnvelope    fade_;
    OokGate         gate_;
    AnalogFrontEnd  front_;
    AdcQuantizer    adc_;
};

} // namespace sigsim
