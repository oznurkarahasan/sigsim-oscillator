Phase 3 not fully complete — core detectors work, but 4 acceptance-test checks are still failing. Quick status:

Done:

Fixed the environment (static-linked libstdc++, installed numpy) — original 6 checks all pass.
Built full Phase 3 skeleton: DetectorTypes.h, IDetector.h, PeakTracker.h, BiquadDetector.h, GoertzelDetector.h, AutocorrDetector.h, BitScorer.h, detect_dump.cpp, detect_sweep.cpp, CMake wiring.
Found and fixed 3 real bugs the plan's spec didn't anticipate:
Goertzel/autocorrelation were reading raw ADC voltage including its 1.6V DC offset, drowning the 20mV carrier → added AC-coupling to both.
BitScorer's 20%-guard-band couldn't absorb a full-symbol block/window latency → added exact delay compensation.
Autocorrelation's default 1ms window couldn't flush between OOK pulses → shrank default to 0.25ms.
Biggest one: PeakTracker's decay constant assumed per-sample calls, but Goertzel only calls it once per block — silently making its 20ms tau effectively ~4 seconds. Fixed by calibrating against the actual call rate.
Result: detect_sweep now shows correct, monotonic BER-vs-noise for all three detectors at default params.
Extended tools/verify.py with the 4 Phase-3 acceptance checks from §3.9.
Just found, not yet fixed: running that extended verify.py surfaced 4 failing checks — an off-target frequency isn't being suppressed by biquad/Goertzel as expected, and autocorrelation's on-target correlation (0.83) is lower than integer-lag (0.95), the reverse of what the fractional-lag fix should show. These look like real bugs in my checks or detectors, still need to debug.