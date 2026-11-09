#ifndef AIRITL_NATIVE_WCL_H
#define AIRITL_NATIVE_WCL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Pinned 24G720 WCL producer/consumer byte contracts. Reserved fields retain
// their original bytes; no legacy ioctl structure aliases these carriers.
struct AiritlWclChannel { uint32_t version, channel, flags; };
struct AiritlWclSsid {
    uint32_t flags, length;
    uint8_t bytes[32];
    uint8_t reserved[16];
};
struct apple80211ScanRequest {
    uint32_t header;
    uint8_t privateScan, reserved05[3];
    uint32_t homeAway, priority, bssType;
    uint8_t bssid[6], reserved1a[2];
    uint32_t ssidLength;
    uint8_t ssid[32];
    uint32_t mode;
    uint16_t flags, reserved46;
    uint32_t activeDwell, passiveDwell, homeDwell, channelCount;
    AiritlWclChannel channels[400];
    uint32_t ssidCount;
    AiritlWclSsid ssids[10];
    uint32_t shortSsid;
};
struct AiritlWclCandidate {
    uint8_t flags, reserved01;
    uint16_t info;
    uint8_t bssid[6], mldAddress[6];
    uint16_t channelSpec;
};
struct apple80211AssocCandidates {
    uint8_t header[4];
    uint32_t unassociatedDwell, batteryMode;
    uint16_t apMode, reserved0e;
    uint32_t lowerAuth, upperAuth, extraAuth, ssidLength;
    uint8_t ssid[32], key[148];
    uint16_t rsnLength;
    uint8_t rsn[257], mldAddress[6], reserved1dd[3];
    uint16_t flags, reserved1e2;
    uint32_t capabilities, securityFlags, secretLength;
    uint8_t secret[32];
    uint32_t cipher, candidateCount;
    AiritlWclCandidate candidates[10];
    uint16_t rsnxeLength;
    uint8_t rsnxe[257], reserved3cf;
    uint32_t policy, tailPolicy[2];
};
static_assert(sizeof(apple80211ScanRequest) == 0x1550, "WCL scan size");
static_assert(offsetof(apple80211ScanRequest, channelCount) == 0x54, "WCL channel count");
static_assert(offsetof(apple80211ScanRequest, channels) == 0x58, "WCL channel array");
static_assert(offsetof(apple80211ScanRequest, ssidCount) == 0x1318, "WCL SSID count");
static_assert(offsetof(apple80211ScanRequest, shortSsid) == 0x154c, "WCL short SSID");
static_assert(sizeof(AiritlWclCandidate) == 0x12, "WCL candidate stride");
static_assert(sizeof(apple80211AssocCandidates) == 0x3dc, "WCL association size");
static_assert(offsetof(apple80211AssocCandidates, key) == 0x40, "WCL key offset");
static_assert(offsetof(apple80211AssocCandidates, rsnLength) == 0xd4, "WCL RSN offset");
static_assert(offsetof(apple80211AssocCandidates, flags) == 0x1e0, "WCL flags offset");
static_assert(offsetof(apple80211AssocCandidates, candidateCount) == 0x214, "WCL candidates count");
static_assert(offsetof(apple80211AssocCandidates, candidates) == 0x218, "WCL candidates offset");
static_assert(offsetof(apple80211AssocCandidates, rsnxeLength) == 0x2cc, "WCL RSNXE offset");
static_assert(offsetof(apple80211AssocCandidates, policy) == 0x3d0, "WCL policy offset");

struct apple80211_wcl_update_link_state {
    uint8_t bssid[6], linkUp, publishLinkState, updateBss, reserved09[3];
    uint32_t debounceMilliseconds;
};
struct apple80211_leave_network {
    uint32_t leaveReason, enhancedReason;
    uint16_t ieeeReason, waitTime;
    uint32_t linkChangeReason;
    uint8_t methodControl, involuntary, abortJoin, bssid[6], reserved19[3];
};
static_assert(sizeof(apple80211_wcl_update_link_state) == 16, "WCL link publication size");
static_assert(offsetof(apple80211_wcl_update_link_state, debounceMilliseconds) == 12, "WCL debounce offset");
static_assert(sizeof(apple80211_leave_network) == 28, "WCL leave size");
static_assert(offsetof(apple80211_leave_network, bssid) == 19, "WCL leave BSSID offset");
struct AiritlWclKey {
    uint32_t version, length, cipher;
    uint16_t flags, index;
    uint8_t data[64];
    uint32_t rscLength;
    uint8_t rsc[8], bssid[6], reserved62[2];
    uint8_t opaqueTail[48];
};
static_assert(sizeof(AiritlWclKey) == 148, "Native key size");
static_assert(offsetof(AiritlWclKey, rscLength) == 80, "Native replay counter length");
static_assert(offsetof(AiritlWclKey, bssid) == 92, "Native key BSSID");
struct AiritlWclAuth { uint32_t version, lower, upper, selector; };
static_assert(sizeof(AiritlWclAuth) == 16, "Native authentication carrier");
struct apple80211_low_latency_info {
    uint8_t trafficOngoing, p2pRealtimeOn;
    uint16_t desiredChannel;
};
static_assert(sizeof(apple80211_low_latency_info) == 4, "Native low latency carrier");
namespace AiritlWcl {
struct BeaconInput {
    uint16_t channel;
    uint32_t channelFlags;
    int32_t rssi;
    int16_t noise;
    bool noiseValid;
};
inline uint16_t read16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t read32(const uint8_t *p) { return uint32_t(read16(p)) | uint32_t(read16(p + 2)) << 16; }
inline void write16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void write32(uint8_t *p, uint32_t v) { write16(p, uint16_t(v)); write16(p + 2, uint16_t(v >> 16)); }
// Input is the complete validated 802.11 beacon/probe-response frame. Apple
// receives only the original information elements after its 64-byte metadata.
inline bool encodeBeacon(const uint8_t *frame, size_t length, const BeaconInput &in,
                         uint8_t *out, size_t capacity, size_t &written) {
    written = 0;
    if (!frame || !out || length < 36 || length - 36 > 2048 || capacity < 64 + length - 36 ||
        (frame[0] != 0x80 && frame[0] != 0x50) || !in.channel || in.channel > 255 ||
        !(in.channelFlags & (0x8 | 0x10))) return false;
    uint8_t nonzeroBssid = 0;
    for (unsigned i = 0; i < 6; ++i) nonzeroBssid |= frame[16 + i];
    if (!nonzeroBssid || (frame[16] & 1)) return false;
    const uint8_t *ssid = nullptr;
    uint8_t ssidLength = 0;
    for (size_t i = 36; i < length;) {
        if (length - i < 2 || size_t(frame[i + 1]) > length - i - 2) return false;
        if (frame[i] == 0) {
            if (ssid || frame[i + 1] > 32) return false;
            ssid = frame + i + 2;
            ssidLength = frame[i + 1];
        }
        i += size_t(frame[i + 1]) + 2;
    }
    if (!ssid) return false;
    memset(out, 0, 64);
    write32(out, uint32_t(length - 36));
    write16(out + 4, uint16_t(in.channel | ((in.channelFlags & 0x10) ? 0xd000 : 0x1000)));
    memcpy(out + 6, ssid, ssidLength);
    out[0x26] = ssidLength;
    out[0x27] = uint8_t(in.channel);
    memcpy(out + 0x29, frame + 16, 6);
    write32(out + 0x30, uint32_t(in.rssi));
    uint32_t flags = 0x4002;
    if (in.noiseValid) {
        write16(out + 0x34, uint16_t(in.noise));
        const int32_t snr = in.rssi - in.noise;
        if (snr >= -32768 && snr <= 32767) { write16(out + 0x36, uint16_t(snr)); flags |= 0x2000; }
        flags |= 0x1000;
    }
    memcpy(out + 0x38, frame + 32, 4);
    write32(out + 0x3c, flags);
    memcpy(out + 64, frame + 36, length - 36);
    written = 64 + length - 36;
    return true;
}
inline void encodeAuthAssoc(uint8_t (&out)[28], const uint8_t *bssid, uint16_t status,
                            uint16_t extended, uint32_t firmwareStatus, uint32_t reason) {
    memset(out, 0, sizeof(out)); write16(out, status); write16(out + 2, extended);
    out[4] = 1; memcpy(out + 5, bssid, 6);
    write32(out + 20, firmwareStatus); write32(out + 24, reason);
}
inline void encodeFirstBeacon(uint8_t (&out)[20], const uint8_t *bssid) {
    memset(out, 0, sizeof(out)); memcpy(out + 4, bssid, 6);
}
inline void encodeConnect(uint8_t (&out)[164], const uint8_t *bssid, uint16_t status,
                          uint16_t extended, uint32_t firmwareStatus, uint32_t reason) {
    memset(out, 0, sizeof(out)); write16(out, status); write16(out + 2, extended);
    memcpy(out + 4, bssid, 6); write32(out + 12, firmwareStatus); write32(out + 16, reason);
}
enum class Error { none, length, count, ssid, channel, mode, informationElement, security, unsupported };
inline Error copyKey(const void *input, size_t length, AiritlWclKey &out) {
    if (!input || length != sizeof(out)) return Error::length;
    memcpy(&out, input, sizeof(out));
    if (out.length > 64 || out.rscLength > 8)
        return Error::length;
    return Error::none;
}
inline uint32_t scanStatus(bool success, bool cancelled, bool timedOut) {
    return success ? 0 : (cancelled ? 0xe0820405U : (timedOut ? 0xe0820402U : 0xe0820401U));
}

inline bool validIe(const uint8_t *ie, uint32_t length, uint32_t capacity) {
    return length == 0 || (length >= 2 && length <= capacity && uint32_t(ie[1]) + 2 == length);
}
enum class SecurityProtocol { none, wpa, rsn };
enum class SecurityCipher { none = 0, tkip = 1, ccmp = 2 };
enum class SecurityAkm { none = 0, psk = 1, sha256Psk = 2 };
struct SecurityPolicy {
    SecurityProtocol protocol = SecurityProtocol::none;
    SecurityCipher group = SecurityCipher::none;
    uint32_t pairwiseCiphers = 0, akms = 0;
    uint16_t capabilities = 0;
};
// The WCL RSN/WPA carrier contains the complete IE, including ID and length.
// This adapter implements the existing non-PMF PSK profile. It does not silently
// discard requested RSNXE, PMKSA or required management protection. Suite lists
// are offers; selection stays within their supported intersection.
inline Error joinSecurityPolicy(const apple80211AssocCandidates &request, SecurityPolicy &out) {
    out = {};
    if (!validIe(request.rsn, request.rsnLength, sizeof(request.rsn)) ||
        !validIe(request.rsnxe, request.rsnxeLength, sizeof(request.rsnxe))) return Error::informationElement;
    if (request.rsnxeLength || request.secretLength) return Error::unsupported;
    // Native BCM consumes these bits as SAE-PK and operating-channel validation.
    if (request.capabilities & 0x21) return Error::unsupported;
    const uint32_t pmfMode = request.capabilities & 0x0e;
    if (pmfMode && pmfMode != 2 && pmfMode != 4 && pmfMode != 8) return Error::security;
    if (pmfMode == 8) return Error::unsupported;
    if (!request.rsnLength) return Error::none;
    if (!request.upperAuth) return Error::security;
    const uint8_t *cursor = request.rsn + 2;
    const uint8_t *end = request.rsn + request.rsnLength;
    bool wpa = request.rsn[0] == 221;
    const uint8_t rsnOui[3] = {0, 0x0f, 0xac}, wpaOui[3] = {0, 0x50, 0xf2};
    const uint8_t *oui = wpa ? wpaOui : rsnOui;
    if (wpa) {
        if (end - cursor < 4 || memcmp(cursor, wpaOui, 3) || cursor[3] != 1) return Error::informationElement;
        cursor += 4;
    } else if (request.rsn[0] != 48) return Error::informationElement;
    auto take = [&](size_t count) -> const uint8_t * {
        if (size_t(end - cursor) < count) return nullptr;
        const uint8_t *bytes = cursor; cursor += count; return bytes;
    };
    auto cipher = [&](const uint8_t *suite) -> SecurityCipher {
        if (memcmp(suite, oui, 3)) return SecurityCipher::none;
        return suite[3] == 2 ? SecurityCipher::tkip : suite[3] == 4 ? SecurityCipher::ccmp : SecurityCipher::none;
    };
    const uint8_t *bytes = take(2);
    if (!bytes || read16(bytes) != 1) return Error::informationElement;
    bytes = take(4);
    if (!bytes) return Error::informationElement;
    out.group = cipher(bytes);
    if (out.group == SecurityCipher::none) return Error::unsupported;
    bytes = take(2);
    if (!bytes) return Error::informationElement;
    uint16_t count = read16(bytes);
    if (!count || count > size_t(end - cursor) / 4) return Error::informationElement;
    for (unsigned i = 0; i < count; ++i) {
        const SecurityCipher selected = cipher(take(4));
        out.pairwiseCiphers |= uint32_t(selected);
    }
    if (!out.pairwiseCiphers) return Error::unsupported;
    bytes = take(2);
    if (!bytes) return Error::informationElement;
    count = read16(bytes);
    if (!count || count > size_t(end - cursor) / 4) return Error::informationElement;
    for (unsigned i = 0; i < count; ++i) {
        bytes = take(4);
        if (!memcmp(bytes, oui, 3)) {
            if (bytes[3] == 2) out.akms |= uint32_t(SecurityAkm::psk);
            else if (!wpa && bytes[3] == 6) out.akms |= uint32_t(SecurityAkm::sha256Psk);
        }
    }
    if (!out.akms) return Error::unsupported;
    out.protocol = wpa ? SecurityProtocol::wpa : SecurityProtocol::rsn;
    uint32_t allowedAkms = 0;
    if (request.upperAuth & (wpa ? 2U : 8U)) allowedAkms |= uint32_t(SecurityAkm::psk);
    if (!wpa && (request.upperAuth & 0x400U)) allowedAkms |= uint32_t(SecurityAkm::sha256Psk);
    out.akms &= allowedAkms;
    if (!out.akms) return Error::security;
    // The admitted SHA256 PTK profile has a 16-byte CCMP temporal key.
    if (!(out.pairwiseCiphers & uint32_t(SecurityCipher::ccmp)))
        out.akms &= ~uint32_t(SecurityAkm::sha256Psk);
    if (!out.akms) return Error::unsupported;
    if (cursor == end) return Error::none;
    bytes = take(2);
    if (!bytes) return Error::informationElement;
    out.capabilities = read16(bytes);
    if ((out.capabilities & 0xc0) == 0x40) return Error::security;
    if (out.capabilities & 0x40) return Error::unsupported;
    if (out.capabilities & ~uint16_t(wpa ? 0 : 0xbc)) return Error::unsupported;
    if (wpa) return cursor == end ? Error::none : Error::unsupported;
    if (cursor == end) return Error::none;
    bytes = take(2);
    if (!bytes) return Error::informationElement;
    count = read16(bytes);
    if (count > size_t(end - cursor) / 16) return Error::informationElement;
    if (count) return Error::unsupported;
    if (cursor == end) return Error::none;
    bytes = take(4);
    if (!bytes) return Error::informationElement;
    if (memcmp(bytes, rsnOui, 3) || bytes[3] != 6) return Error::unsupported;
    return cursor == end ? Error::none : Error::unsupported;
}
inline Error copyScan(const void *input, size_t length, apple80211ScanRequest &out) {
    if (!input || length != sizeof(out)) return Error::length;
    memcpy(&out, input, sizeof(out));
    if (out.channelCount > 400 || out.ssidCount > 10) return Error::count;
    if (out.ssidLength > 32) return Error::ssid;
    if (out.mode < 1 || out.mode > 4) return Error::mode;
    for (uint32_t i = 0; i < out.channelCount; ++i)
        if (!out.channels[i].channel || out.channels[i].channel > 255) return Error::channel;
    for (uint32_t i = 0; i < out.ssidCount; ++i)
        if (out.ssids[i].length > 32) return Error::ssid;
    return Error::none;
}
inline Error copyJoin(const void *input, size_t length, apple80211AssocCandidates &out) {
    if (!input || length != sizeof(out)) return Error::length;
    memcpy(&out, input, sizeof(out));
    if (!out.candidateCount || out.candidateCount > 10) return Error::count;
    if (!out.ssidLength || out.ssidLength > 32) return Error::ssid;
    if (out.apMode > 3) return Error::mode;
    if (out.secretLength > sizeof(out.secret)) return Error::length;
    AiritlWclKey key;
    const Error keyError = copyKey(out.key, sizeof(out.key), key);
    memset(&key, 0, sizeof(key));
    if (keyError != Error::none) return Error::length;
    if (!validIe(out.rsn, out.rsnLength, sizeof(out.rsn)) ||
        !validIe(out.rsnxe, out.rsnxeLength, sizeof(out.rsnxe))) return Error::informationElement;
    return Error::none;
}
// A completion must match both operation identity and generation. Cancellation
// invalidates a saved token before another operation may acquire the state.
struct RequestState {
    uint64_t generation, drainingGeneration;
    bool active, draining;
    RequestState() : generation(0), drainingGeneration(0), active(false), draining(false) {}
    uint64_t begin() { if (active || draining) return 0; active = true; if (++generation == 0) ++generation; return generation; }
    bool finish(uint64_t token) {
        if (draining && token == drainingGeneration) { draining = false; drainingGeneration = 0; return false; }
        if (!active || token != generation) return false;
        active = false; return true;
    }
    uint64_t cancel() {
        const uint64_t token = active ? generation : 0;
        if (active) { draining = true; drainingGeneration = generation; }
        active = false; if (++generation == 0) ++generation; return token;
    }
    void quiesced() { active = false; draining = false; drainingGeneration = 0; if (++generation == 0) ++generation; }
};
struct ScanRequestState : RequestState {
    bool submitted, cancelling;
    uint64_t deadline;
    ScanRequestState() : submitted(false), cancelling(false), deadline(0) {}
    uint64_t beginQueued(uint64_t limit) {
        const uint64_t token = RequestState::begin();
        if (token) { submitted = false; cancelling = false; deadline = limit; }
        return token;
    }
    bool markSubmitted(uint64_t token) {
        if (!active || token != generation || submitted) return false;
        submitted = true; return true;
    }
    void retryQueued(uint64_t token) { if (active && token == generation) submitted = false; }
    bool requestCancel(uint64_t token) {
        if (!active || !submitted || token != generation || cancelling) return false;
        cancelling = true; return true;
    }
    void cancelRejected(uint64_t token) { if (active && token == generation) cancelling = false; }
    bool queueExpired(uint64_t now) const { return active && !submitted && now >= deadline; }
    bool finish(uint64_t token) {
        const bool terminal = RequestState::finish(token);
        if (terminal || (!active && !draining)) { submitted = false; cancelling = false; deadline = 0; }
        return terminal;
    }
    uint64_t cancel() {
        const bool wasSubmitted = submitted;
        const uint64_t token = RequestState::cancel();
        if (token && !wasSubmitted) RequestState::finish(token);
        submitted = false; cancelling = false; deadline = 0;
        return token;
    }
    void quiesced() { RequestState::quiesced(); submitted = false; cancelling = false; deadline = 0; }
};

}
#endif
