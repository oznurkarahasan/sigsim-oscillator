// CLI test harness for the signal-chain simulation. No GUI dependency --
// the point is to validate the oscillator/noise/fade/RC/ADC chain
// headlessly (CSV out, or a --sweep mode) before any detector or GUI
// code is built on top of it.

#include "sigsim/SignalChain.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace sigsim;

namespace {

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "  --duration-ms <ms>   length of run to dump, in ms (default 20)\n"
        "  --out <file.csv>     output file (default: stdout)\n"
        "\n"
        "  --fs <Hz>            ADC sample rate (default 200000)\n"
        "  --oversample <N>     internal analog-domain oversample factor (default 16)\n"
        "  --bits <n>           ADC resolution in bits (default 12)\n"
        "  --vref <V>           ADC reference voltage (default 3.3)\n"
        "\n"
        "  --noise-mv <mV>      white noise RMS at the ADC pin (default 40)\n"
        "  --rc-cutoff <Hz>     analog RC low-pass cutoff before the ADC (default 100000)\n"
        "\n"
        "  --sig-freq <Hz>      carrier frequency (default 47400)\n"
        "  --sig-amp-mv <mV>    nominal (unfaded) carrier amplitude (default 20)\n"
        "  --offset-v <V>       DC offset (default 1.6)\n"
        "\n"
        "  --no-fade            disable amplitude fading (multiplier stays 1.0)\n"
        "  --fade-update-ms <ms> fade random-walk keyframe interval (default 50)\n"
        "  --fade-step <x>      fade random-walk step size, normalized space (default 0.08)\n"
        "  --fade-min <x>       min fade amplitude multiplier (default 0.05)\n"
        "  --fade-max <x>       max fade amplitude multiplier (default 120)\n"
        "\n"
        "  --no-ook             disable on/off keying (carrier always on)\n"
        "  --ook-on-ms <ms>     OOK on-time (default 1.0)\n"
        "  --ook-off-ms <ms>    OOK off-time (default 1.0)\n"
        "\n"
        "  --seed <n>           RNG seed (default 12345)\n"
        "  --help               show this message\n";
}

double argD(int argc, char** argv, int& i) {
    if (i + 1 >= argc) {
        std::cerr << "missing value for " << argv[i] << "\n";
        std::exit(1);
    }
    return std::atof(argv[++i]);
}

std::string argS(int argc, char** argv, int& i) {
    if (i + 1 >= argc) {
        std::cerr << "missing value for " << argv[i] << "\n";
        std::exit(1);
    }
    return std::string(argv[++i]);
}

} // namespace

int main(int argc, char** argv) {
    Params p;
    double durationMs = 20.0;
    std::string outPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        else if (a == "--duration-ms")   durationMs = argD(argc, argv, i);
        else if (a == "--out")           outPath = argS(argc, argv, i);
        else if (a == "--fs")            p.sampleRateHz = argD(argc, argv, i);
        else if (a == "--oversample")    p.oversample = static_cast<int>(argD(argc, argv, i));
        else if (a == "--bits")          p.adcBits = static_cast<int>(argD(argc, argv, i));
        else if (a == "--vref")          p.vrefV = argD(argc, argv, i);
        else if (a == "--noise-mv")      p.noiseRmsMv = argD(argc, argv, i);
        else if (a == "--rc-cutoff")     p.rcCutoffHz = argD(argc, argv, i);
        else if (a == "--sig-freq")      p.signalFreqHz = argD(argc, argv, i);
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
        else {
            std::cerr << "unknown option: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    SignalChain chain;
    chain.configure(p);

    std::ofstream fout;
    std::ostream* out = &std::cout;
    if (!outPath.empty()) {
        fout.open(outPath);
        if (!fout) {
            std::cerr << "cannot open output file: " << outPath << "\n";
            return 1;
        }
        out = &fout;
    }

    *out << "time_s,ideal_v,pre_adc_v,adc_code,adc_v,bit_truth,fade_mult\n";

    const long nSamples = static_cast<long>(durationMs / 1000.0 * p.sampleRateHz);
    for (long n = 0; n < nSamples; ++n) {
        const AdcSample s = chain.step();
        *out << s.timeS << ',' << s.idealV << ',' << s.preAdcV << ','
             << s.code << ',' << s.adcV << ',' << (s.bitTruth ? 1 : 0) << ','
             << s.fadeMult << '\n';
    }

    return 0;
}
