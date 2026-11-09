#ifndef AIRITL_IO80211_FAULT_REPORTER_H
#define AIRITL_IO80211_FAULT_REPORTER_H

#include "CCStream.h"
#include <IOKit/IOWorkLoop.h>

// Factory-only declarations from the pinned 24G720 images. These objects must
// come from the native factories. No local allocation, fields or subclassing.
class CCDataStream : public CCStream {
private:
    CCDataStream() = delete;
};

class CCFaultReporter : public IOService {
public:
    static CCFaultReporter *withStreamWorkloop(CCDataStream *, IOWorkLoop *);
private:
    CCFaultReporter() = delete;
};

class IO80211FaultReporter : public OSObject {
public:
    static IO80211FaultReporter *allocWithParams(CCFaultReporter *);
private:
    IO80211FaultReporter() = delete;
};

#endif
