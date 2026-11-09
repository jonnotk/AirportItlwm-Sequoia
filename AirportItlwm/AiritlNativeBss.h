#ifndef AIRITL_NATIVE_BSS_H
#define AIRITL_NATIVE_BSS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct apple80211_beacon_msg { uint8_t bytes[0x840]; };
struct AiritlNativeRate { uint32_t version, rate, flags; };
struct AiritlNativeRates {
    uint32_t version;
    uint16_t count, reserved;
    AiritlNativeRate rates[15];
};
struct AiritlNativeHtMap { uint32_t version; uint8_t map[10], reserved[2]; };
struct AiritlNativeMcsMap { uint32_t version; uint16_t map, reserved; };
struct apple80211_extended_bss_info {
    AiritlNativeRates rates;
    AiritlNativeHtMap ht;
    AiritlNativeMcsMap vht, he;
    uint8_t mlo, associationIe[257], reserved[2];
};
struct apple80211_fastlane { uint8_t capable, overrideAdmissionControl; };
static_assert(sizeof(apple80211_beacon_msg) == 0x840, "Native BSS info carrier");
static_assert(sizeof(AiritlNativeRates) == 0xbc, "Native current rate set");
static_assert(sizeof(apple80211_extended_bss_info) == 0x1e0, "Native extended BSS info");
static_assert(offsetof(apple80211_extended_bss_info, ht) == 0xbc, "Native HT map");
static_assert(offsetof(apple80211_extended_bss_info, vht) == 0xcc, "Native VHT map");
static_assert(offsetof(apple80211_extended_bss_info, he) == 0xd4, "Native HE map");
static_assert(offsetof(apple80211_extended_bss_info, associationIe) == 0xdd, "Native STA IE");
static_assert(sizeof(apple80211_fastlane) == 2, "Native Fast Lane carrier");

namespace AiritlWcl {
template<class CurrentRates>
inline bool encodeExtendedBss(const CurrentRates &rates, const uint8_t *ie, size_t ieLength,
                              apple80211_extended_bss_info &output) {
    if (rates.legacyCount > 15 || ieLength > sizeof(output.associationIe) ||
        (ieLength && (!ie || ieLength < 2 || size_t(ie[1]) + 2 != ieLength))) return false;
    apple80211_extended_bss_info value = {};
    value.rates.count = rates.legacyCount;
    for (unsigned i = 0; i < rates.legacyCount; ++i) {
        value.rates.rates[i].rate = (rates.legacy[i] >> 1) & 0x3f;
        value.rates.rates[i].flags = rates.legacy[i] >> 7;
    }
    memcpy(value.ht.map, rates.ht, sizeof(value.ht.map));
    value.vht.map = rates.vht;
    value.he.map = rates.he;
    if (ieLength) memcpy(value.associationIe, ie, ieLength);
    output = value;
    return true;
}
inline void encodeLinkStatus(uint8_t (&output)[12], const uint8_t *bssid,
                             bool up, bool debounce) {
    memset(output, 0, sizeof(output));
    memcpy(output, bssid, 6);
    output[6] = up;
    output[7] = debounce;
    // Native maps an unavailable vendor firmware link reason to 0xff.
    output[8] = 0xff;
}
}
#endif
