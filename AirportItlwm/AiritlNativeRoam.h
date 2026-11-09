#ifndef AIRITL_NATIVE_ROAM_H
#define AIRITL_NATIVE_ROAM_H
#include "AiritlNativeWcl.h"
struct apple80211_reassoc {
    uint16_t channels[50];
    uint8_t bssids[7][6];
    uint16_t reserved8e;
    uint32_t bssidCount, channelCount;
    uint8_t flags, ssid[32], ssidLength, reservedba[2];
};
static_assert(sizeof(apple80211_reassoc) == 0xbc, "Native reassociation carrier");
static_assert(offsetof(apple80211_reassoc, bssidCount) == 0x90, "Native reassociation BSSIDs");
static_assert(offsetof(apple80211_reassoc, ssid) == 0x99, "Native reassociation SSID");
struct AiritlNativeRoamState {
    apple80211_reassoc request;
    uint64_t generation, deadline;
    bool active, associationReported, firstBeacon, draining, hardwareQuiesced, downAcknowledged;
    uint8_t origin[6], target[6];
};
namespace AiritlWcl {
inline bool validRoamRequest(const apple80211_reassoc &r) {
    if (!r.bssidCount || r.bssidCount > 7 || r.channelCount > 50 || !r.ssidLength || r.ssidLength > 32) return false;
    for (unsigned i = 0; i < r.bssidCount; ++i) {
        uint8_t any = 0, all = 0xff;
        for (unsigned j = 0; j < 6; ++j) { any |= r.bssids[i][j]; all &= r.bssids[i][j]; }
        if (!any || ((r.bssids[i][0] & 1) && all != 0xff)) return false;
    }
    return true;
}
inline bool roamBssidAllowed(const apple80211_reassoc &r, const uint8_t *bssid) {
    for (unsigned i = 0; i < r.bssidCount; ++i) {
        uint8_t all = 0xff;
        for (unsigned j = 0; j < 6; ++j) all &= r.bssids[i][j];
        if (all == 0xff || !memcmp(r.bssids[i], bssid, 6)) return true;
    }
    return false;
}
inline void encodeRoamStatus(uint8_t (&out)[168], uint32_t status) {
    memset(out, 0, sizeof(out)); write32(out, status); write32(out + 4, 0xe3ff8100);
    // Native roamDone overlays its real104-byte metrics before public delivery.
    out[0x68] = 1;
}
}
#endif
