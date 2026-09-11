// BER-vs-parameter sweep, headless -- this is the actual comparison
// deliverable (the GUI in Phase 4 is for eyeballing behavior; this answers
// "which detector wins" quantitatively).
//
// For each swept value: reconfigure SignalChain (holding every other
// sigsim::Params at its default), run --symbols-per-point BitScorer
// symbols through all three detectors in parallel (one SignalChain::step()
// feeds all three processSample() calls each tick), score each with its
// own BitScorer, write one CSV row: sweep_value,ber_biquad,ber_goertzel,
// ber_autocorr.
//
// A "symbol" (see detect/BitScorer.h) is one on-pulse or one off-gap, not
// a full on+off cycle -- that's the only definition compatible with the
// guard-band majority vote, since a full cycle straddles the truth
// transition. --symbols-per-point counts these half-period symbols.

#include "sigsim/SignalChain.h"
#include "detect/BiquadDetector.h"
#include "detect/GoertzelDetector.h"
#include "detect/AutocorrDetector.h"
#include "detect/BitScorer.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace sigsim;
using namespace detect;

namespace {

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " --sweep-param <noise-mv|fade-max> --values <v1,v2,...> [options]\n"
        "\n"
        "  --sweep-param <name>     parameter to sweep: noise-mv or fade-max\n"
        "  --values <csv>           comma-separated list of values to test\n"
        "  --symbols-per-point <n>  BitScorer symbols to run per point (default 2000)\n"
        "  --seed <n>               RNG seed, same for every point (default 1)\n"
        "  --out <file.csv>         output file (default: stdout)\n"
        "\n"
        "  Detector tuning: --threshold-frac, --peak-tau-ms, --biquad-q,\n"
        "  --biquad-env-tau-ms, --goertzel-block-ms, --autocorr-window-ms\n"
        "  (see detect_dump --help for these)\n"
        "\n"
        "  --help                   show this message\n";
}

double argD(int argc, char** argv, int& i) {
    if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; std::exit(1); }
    return std::atof(argv[++i]);
}

std::string argS(int argc, char** argv, int& i) {
    if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; std::exit(1); }
    return std::string(argv[++i]);
}

std::vector<double> parseValues(const std::string& csv) {
    std::vector<double> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(std::atof(tok.c_str()));
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string sweepParam;
    std::vector<double> values;
    long symbolsPerPoint = 2000;
    uint32_t seed = 1;
    std::string outPath;
    DetectorParams dp;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        else if (a == "--sweep-param")        sweepParam = argS(argc, argv, i);
        else if (a == "--values")             values = parseValues(argS(argc, argv, i));
        else if (a == "--symbols-per-point")  symbolsPerPoint = static_cast<long>(argD(argc, argv, i));
        else if (a == "--seed")               seed = static_cast<uint32_t>(argD(argc, argv, i));
        else if (a == "--out")                outPath = argS(argc, argv, i);
        else if (a == "--threshold-frac")     dp.thresholdFrac = argD(argc, argv, i);
        else if (a == "--peak-tau-ms")        dp.peakTauMs = argD(argc, argv, i);
        else if (a == "--biquad-q")           dp.biquadQ = argD(argc, argv, i);
        else if (a == "--biquad-env-tau-ms")  dp.biquadEnvTauMs = argD(argc, argv, i);
        else if (a == "--goertzel-block-ms")  dp.goertzelBlockMs = argD(argc, argv, i);
        else if (a == "--autocorr-window-ms") dp.autocorrWindowMs = argD(argc, argv, i);
        else {
            std::cerr << "unknown option: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (sweepParam.empty() || values.empty()) {
        std::cerr << "--sweep-param and --values are required\n";
        printUsage(argv[0]);
        return 1;
    }
    if (sweepParam != "noise-mv" && sweepParam != "fade-max") {
        std::cerr << "--sweep-param must be one of: noise-mv, fade-max\n";
        return 1;
    }

    std::ofstream fout;
    std::ostream* out = &std::cout;
    if (!outPath.empty()) {
        fout.open(outPath);
        if (!fout) { std::cerr << "cannot open output file: " << outPath << "\n"; return 1; }
        out = &fout;
    }
    *out << "sweep_value,ber_biquad,ber_goertzel,ber_autocorr\n";

    for (double value : values) {
        Params p; // defaults, per-point: only the swept field changes
        p.rngSeed = seed;
        dp.sampleRateHz = p.sampleRateHz;
        dp.targetFreqHz = p.signalFreqHz;

        if (sweepParam == "noise-mv") p.noiseRmsMv = value;
        else                          p.fadeMaxMult = value;

        SignalChain chain;
        chain.configure(p);

        BiquadDetector biquad;
        GoertzelDetector goertzel;
        AutocorrDetector autocorr;
        biquad.configure(dp);
        goertzel.configure(dp);
        autocorr.configure(dp);

        // Compensate each detector's own known decision latency before the
        // guard band runs (see BitScorer.h) -- without this, Goertzel and
        // autocorrelation score near chance-level BER regardless of noise,
        // because their block/window length defaults to a full symbol.
        BitScorer scoreBiquad, scoreGoertzel, scoreAutocorr;
        scoreBiquad.configure(p.sampleRateHz, p.ookOnMs, p.ookOffMs, 0.2, 0);
        scoreGoertzel.configure(p.sampleRateHz, p.ookOnMs, p.ookOffMs, 0.2, goertzel.blockSamples());
        scoreAutocorr.configure(p.sampleRateHz, p.ookOnMs, p.ookOffMs, 0.2, autocorr.windowSamples() / 2);

        // Discard a startup transient before scoring begins, same pattern
        // tools/verify.py already uses for its own RMS/aliasing checks. The
        // RC filter charges up from 0V and PeakTracker's very first reading
        // (before the signal chain has settled) can be an outlier -- with
        // peakTauMs's slow decay (deliberately slow, so it doesn't chase
        // individual noise spikes or symbol-to-symbol variation) a single
        // inflated startup peak can misnormalize every decision for a long
        // stretch afterward if it's allowed to seed the AGC. Run (but don't
        // score) enough samples for the slowest time constant in play --
        // the peak tracker's -- to settle first.
        const long warmupSamples = static_cast<long>(std::lround(5.0 * dp.peakTauMs / 1000.0 * p.sampleRateHz));
        for (long i = 0; i < warmupSamples; ++i) {
            const AdcSample s = chain.step();
            biquad.processSample(s.adcV);
            goertzel.processSample(s.adcV);
            autocorr.processSample(s.adcV);
        }

        while (scoreBiquad.totalSymbols() < static_cast<uint64_t>(symbolsPerPoint)) {
            const AdcSample s = chain.step();
            const DetectorOutput ob = biquad.processSample(s.adcV);
            const DetectorOutput og = goertzel.processSample(s.adcV);
            const DetectorOutput oa = autocorr.processSample(s.adcV);

            bool correct;
            scoreBiquad.update(ob.bitDecision, s.bitTruth, correct);
            scoreGoertzel.update(og.bitDecision, s.bitTruth, correct);
            scoreAutocorr.update(oa.bitDecision, s.bitTruth, correct);
        }

        *out << value << ',' << scoreBiquad.ber() << ',' << scoreGoertzel.ber() << ',' << scoreAutocorr.ber() << '\n';
    }

    return 0;
}
