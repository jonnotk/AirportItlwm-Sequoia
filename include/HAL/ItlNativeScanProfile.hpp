/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ITL_NATIVE_SCAN_PROFILE_HPP
#define ITL_NATIVE_SCAN_PROFILE_HPP

#include <stdint.h>

// An owned HAL request. Channel bits use IEEE channel numbers, 1 through 255.
// An empty bitmap selects all available channels. Dwell is in 1024-us TUs.
// Zero dwell keeps the HAL default. Intel's RRM producer and v7/v8 migration
// preserve cfg80211 duration TUs in these firmware byte fields.
struct ItlNativeScanSSID {
    uint8_t len;
    uint8_t bytes[32];
};
struct ItlNativeScanProfile {
    uint64_t token;
    uint8_t passive;
    uint8_t background;
    uint8_t ssidCount;
    uint8_t hasBSSID;
    uint8_t hasScanMac;
    uint16_t activeDwell;
    uint16_t passiveDwell;
    uint16_t homeDwell;
    uint8_t channelBitmap[32];
    ItlNativeScanSSID ssids[10];
    uint8_t bssid[6];
    uint8_t scanMac[6];
};

// Round up so a millisecond request loses no requested listening time.
// The largest encodable dwell is 255 TUs = 261.120 ms.
inline bool itlNativeScanDwellMillisecondsToTU(uint32_t milliseconds,
                                            uint16_t &tu)
{
    const uint64_t converted =
        (static_cast<uint64_t>(milliseconds) * 1000 + 1023) / 1024;
    if (converted > 255)
        return false;
    tu = static_cast<uint16_t>(converted);
    return true;
}

inline bool itlNativeScanProfileValid(const ItlNativeScanProfile &p)
{
    if (!p.token || p.passive > 1 || p.background > 1 || p.hasBSSID > 1 || p.hasScanMac > 1 ||
        p.ssidCount > 10 || p.activeDwell > 255 || p.passiveDwell > 255 || p.homeDwell > 255 ||
        (p.channelBitmap[0] & 1))
        return false;
    for (unsigned i = 0; i < p.ssidCount; ++i)
        if (p.ssids[i].len > 32)
            return false;
    if (p.hasBSSID) {
        uint8_t any = 0;
        for (unsigned i = 0; i < 6; ++i)
            any |= p.bssid[i];
        if (!any || (p.bssid[0] & 1))
            return false;
    }
    if (p.hasScanMac && (p.scanMac[0] & 3) != 2)
        return false;
    return true;
}
inline bool itlNativeScanHasChannels(const ItlNativeScanProfile &p)
{
    uint8_t any = 0;
    for (unsigned i = 0; i < sizeof(p.channelBitmap); ++i)
        any |= p.channelBitmap[i];
    return any != 0;
}
inline bool itlNativeScanSelectsChannel(const ItlNativeScanProfile &p,
                                      unsigned channel)
{
    return channel > 0 && channel < 256 &&
        (!itlNativeScanHasChannels(p) ||
         (p.channelBitmap[channel / 8] & (1U << (channel % 8))));
}
inline uint32_t itlNativeScanSSIDMask(const ItlNativeScanProfile &p,
                                    bool channelPassive)
{
    if (p.passive || channelPassive)
        return 0;
    // An active wildcard scan still needs the empty SSID in slot zero.
    unsigned count = p.ssidCount ? p.ssidCount : 1;
    return (1U << count) - 1;
}

// Completion contains the token copied when this firmware UID was submitted.
// fwStatus is Intel's final status byte, or zero for local teardown.
typedef void (*ItlNativeScanCompletion)(void *, uint64_t, int32_t, uint8_t);

#endif
