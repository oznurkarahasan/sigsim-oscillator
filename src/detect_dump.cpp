// CLI test harness: runs SignalChain + all three detectors together and
// dumps one CSV row per ADC sample. Lets you eyeball what each detector
// sees against ground truth before the GUI (Phase 4) exists, and doubles
// as the harness tools/verify.py drives for the Phase 3 analytic checks
// (3.9) -- synthetic tones via --noise-mv 0 / transparent RC cutoff, same
// pattern sigsim_dump's own verify.py checks already use.

#include "sigsim/SignalChain.h"
#include "detect/BiquadDetector.h"
#include "detect/GoertzelDetector.h"
#include "detect/AutocorrDetector.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace sigsim;
using namespace detect;

namespace {

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "  --duration-ms <ms>   length of run to dump, in ms (default 20)\n"
        "  --out <file.csv>     output file (default: stdout)\n"
        "\n"
        "  Signal-chain options: same flags as sigsim_dump --help\n"
        "  (--fs, --oversample, --bits, --vref, --noise-mv, --rc-cutoff,\n"
        "   --sig-freq, --sig-amp-mv, --offset-v, --no-fade, --fade-*,\n"
        "   --no-ook, --ook-on-ms, --ook-off-ms, --seed)\n"
        "\n"
        "  --threshold-frac <x>     shared detector threshold (default 0.5)\n"
        "  --peak-tau-ms <ms>       peak-tracker decay time constant (default 20)\n"
        "  --biquad-q <Q>           biquad bandpass Q (default 8)\n"
        "  --biquad-env-tau-ms <ms> biquad envelope-follower tau (default 0.3)\n"
        "  --goertzel-block-ms <ms> Goertzel block length (default 1.0)\n"
        "  --autocorr-window-ms <ms> autocorrelation window (default 0.25)\n"
        "  --autocorr-integer-lag   test-only: disable fractional-lag interpolation\n"
        "\n"
        "  --help               show this message\n";
}

double argD(int argc, char** argv, int& i) {
    if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; std::exit(1); }
    return std::atof(argv[++i]);
}

std::string argS(int argc, char** argv, int& i) {
    if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; std::exit(1); }
    return std::string(argv[++i]);
}

} // namespace

int main(int argc, char** argv) {
    Params p;
    DetectorParams dp;
    double durationMs = 20.0;
    std::string outPath;
    bool autocorrIntegerLag = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        else if (a == "--duration-ms")   durationMs = argD(argc, argv, i);
        else if (a == "--out")           outPath = argS(argc, argv, i);
        else if (a == "--fs")            { p.sampleRateHz = argD(argc, argv, i); dp.sampleRateHz = p.sampleRateHz; }
        else if (a == "--oversample")    p.oversample = static_cast<int>(argD(argc, argv, i));
        else if (a == "--bits")          p.adcBits = static_cast<int>(argD(argc, argv, i));
        else if (a == "--vref")          p.vrefV = argD(argc, argv, i);
        else if (a == "--noise-mv")      p.noiseRmsMv = argD(argc, argv, i);
        else if (a == "--rc-cutoff")     p.rcCutoffHz = argD(argc, argv, i);
        else if (a == "--sig-freq")      { p.signalFreqHz = argD(argc, argv, i); dp.targetFreqHz = p.signalFreqHz; }
        else if (a == "--sig-amp-mv")    p.signalAmplitudeMv = argD(argc, argv, i);
        else if (a == "--offset-v")      p.offsetV = argD(argc, argv, i);
        else if (a == "--no-fade")       p.fadeEnabled = false;
        else if (a == "--fade-update-ms")p.fadeUpdateMs = argD(argc, argv, i);
        else if (a == "--fade-step")     p.fadeStepStd = argD(argc, argv, i);
        else if (a == "--fade-min")      p.fadeMinMult = argD(argc, argv, i);
        else if (a == "--fade-max")      p.fadeMaxMult = argD(argc, argv, i);
        else if (a == "--no-ook")        p.ookEnabled = false;
        else if (a == "--ook-on-ms")     p.ookOnMs = argD(argc, argv, i);
        else if (a == "--ook-off-ms")    p.ookOffMs = argD(argc, argv, i);
        else if (a == "--seed")          p.rngSeed = static_cast<uint32_t>(argD(argc, argv, i));
        else if (a == "--threshold-frac")     dp.thresholdFrac = argD(argc, argv, i);
        else if (a == "--peak-tau-ms")        dp.peakTauMs = argD(argc, argv, i);
        else if (a == "--biquad-q")           dp.biquadQ = argD(argc, argv, i);
        else if (a == "--biquad-env-tau-ms")  dp.biquadEnvTauMs = argD(argc, argv, i);
        else if (a == "--goertzel-block-ms")  dp.goertzelBlockMs = argD(argc, argv, i);
        else if (a == "--autocorr-window-ms") dp.autocorrWindowMs = argD(argc, argv, i);
        else if (a == "--autocorr-integer-lag") autocorrIntegerLag = true;
        else {
            std::cerr << "unknown option: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    SignalChain chain;
    chain.configure(p);

    BiquadDetector biquad;
    GoertzelDetector goertzel;
    AutocorrDetector autocorr;
    biquad.configure(dp);
    goertzel.configure(dp);
    autocorr.configure(dp);
    autocorr.setFractionalLagEnabled(!autocorrIntegerLag);

    std::ofstream fout;
    std::ostream* out = &std::cout;
    if (!outPath.empty()) {
        fout.open(outPath);
        if (!fout) { std::cerr << "cannot open output file: " << outPath << "\n"; return 1; }
        out = &fout;
    }

    *out << "time_s,adc_v,ideal_v,bit_truth,fade_mult,"
            "biquad_raw,biquad_norm,biquad_bit,"
            "goertzel_raw,goertzel_norm,goertzel_bit,"
            "autocorr_raw,autocorr_norm,autocorr_bit\n";

    const long nSamples = static_cast<long>(durationMs / 1000.0 * p.sampleRateHz);
    for (long n = 0; n < nSamples; ++n) {
        const AdcSample s = chain.step();
        const DetectorOutput ob = biquad.processSample(s.adcV);
        const DetectorOutput og = goertzel.processSample(s.adcV);
        const DetectorOutput oa = autocorr.processSample(s.adcV);

        *out << s.timeS << ',' << s.adcV << ',' << s.idealV << ','
             << (s.bitTruth ? 1 : 0) << ',' << s.fadeMult << ','
             << ob.rawMagnitude << ',' << ob.normalizedLevel << ',' << (ob.bitDecision ? 1 : 0) << ','
             << og.rawMagnitude << ',' << og.normalizedLevel << ',' << (og.bitDecision ? 1 : 0) << ','
             << oa.rawMagnitude << ',' << oa.normalizedLevel << ',' << (oa.bitDecision ? 1 : 0) << '\n';
    }

    return 0;
}
