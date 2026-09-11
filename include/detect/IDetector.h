#pragma once
#include "DetectorTypes.h"

namespace detect {

// Common interface all three detectors implement. configure() must be
// safely callable again at runtime with new parameters -- the GUI (Phase 4)
// reconfigures on every slider change, and reconstructing the object each
// time would drop the detector's running state for no reason.
class IDetector {
public:
    virtual void configure(const DetectorParams&) = 0;
    virtual void reset() = 0;
    virtual DetectorOutput processSample(double adcVolts) = 0; // called once per SignalChain::step()
    virtual const char* name() const = 0;
    virtual ~IDetector() = default;
};

} // namespace detect
