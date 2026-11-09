#ifndef AIRITL_NATIVE_CONFIG_H
#define AIRITL_NATIVE_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// Pinned 24G720 GET 0x167 writes version 1 and six address bytes.
struct apple80211_hw_mac_address {
    uint32_t version;
    uint8_t address[6], reserved[2];
};

// GET 0x1c1 is headerless. Its count follows 400 eight-byte entries.
struct AiritlWclChannelInfoEntry {
    uint16_t channelSpec;
    uint8_t sixGHzPower[4];
    uint8_t primaryChannel, flags;
};
struct apple80211ChannelInfo {
    AiritlWclChannelInfoEntry channels[400];
    uint16_t count;
    uint8_t country[3], sixGHzSupported;
};
static_assert(sizeof(apple80211_hw_mac_address) == 12, "Native hardware address size");
static_assert(offsetof(apple80211_hw_mac_address, address) == 4, "Native MAC offset");
static_assert(sizeof(AiritlWclChannelInfoEntry) == 8, "Native channel stride");
static_assert(sizeof(apple80211ChannelInfo) == 0xc86, "Native channel info size");
static_assert(offsetof(apple80211ChannelInfo, count) == 0xc80, "Native channel count offset");
static_assert(offsetof(apple80211ChannelInfo, country) == 0xc82, "Native country offset");
static_assert(offsetof(apple80211ChannelInfo, sixGHzSupported) == 0xc85, "Native 6 GHz offset");

#endif
