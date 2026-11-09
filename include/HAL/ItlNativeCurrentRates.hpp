#ifndef ItlNativeCurrentRates_hpp
#define ItlNativeCurrentRates_hpp

#include <stdint.h>

// The host configuration accepted for submission, not firmware readback.
// Legacy rates retain half-Mbps/basic-bit encoding. MCS maps use IEEE encoding.
struct ItlNativeCurrentRates {
    uint8_t legacyCount;
    uint8_t legacy[15];
    uint8_t ht[10];
    uint16_t vht;
    uint16_t he;
};

#endif
