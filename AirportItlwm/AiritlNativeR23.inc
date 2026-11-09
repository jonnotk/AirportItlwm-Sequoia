#if __IO80211_TARGET >= __MAC_15_0
// Requests WCL sends that our driver previously refused with
// kIOReturnUnsupported, aligned with AppleBCMWLANCore behaviour.
// Offsets are Apple's.
// Where Apple programs firmware and Intel has no equivalent command, we store
// and log the request; the logged values guide any later mapping.

static uint32_t airitlRoamLock;
static uint32_t airitlPmMode;
static uint32_t airitlPowersave;
static uint8_t  airitlQosParams[0x17];
static uint8_t  airitlRoamProfile[0x1c9];

// AppleBCMWLANCore::setWCL_SET_ROAM_LOCK ->
// RoamAdapter::setRoamLock(*param != 0): firmware `roam_off`. Intel firmware
// does not roam by itself; every roam here is started by WCL (setWCL_REASSOC),
// so the lock has nothing further to block. Apple returns 0 on success.
IOReturn AirportItlwmSkywalkInterface::setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *input)
{
    if (!input) return (IOReturn)0x16;   // Apple returns raw 22 for null
    uint32_t word;   // Apple tests the whole word (*param != 0)
    memcpy(&word, input, sizeof(word));
    const uint32_t locked = word != 0;
    if (locked != airitlRoamLock)
        AIRITL_DBG("WCL roam lock: %u -> %u\n", airitlRoamLock, locked);
    airitlRoamLock = locked;
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::setWCL_ROAM_PROFILE_CONFIG: null ->
// 0xe00002bc; per-band profiles at +0, +0x94 (5 GHz), +0x128 (6 GHz) go to
// firmware `join_pref`. No Intel equivalent: store and log.
IOReturn AirportItlwmSkywalkInterface::setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config *input)
{
    if (!input) return (IOReturn)0xe00002bc;
    memcpy(airitlRoamProfile, input, sizeof(airitlRoamProfile));
    const uint8_t *p = airitlRoamProfile;
    AIRITL_DBG("WCL roam profile: 2g %02x%02x%02x%02x%02x%02x%02x%02x 5g %02x%02x%02x%02x%02x%02x%02x%02x flags %02x\n",
          p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
          p[0x94], p[0x95], p[0x96], p[0x97], p[0x98], p[0x99], p[0x9a], p[0x9b], p[0x1c8]);
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::setPM_MODE -> NetAdapter::configurePM: dword at +4,
// firmware PM = (mode != 0) * 2. Our radio
// stays in CAM (ItlIwx.cpp power table). Stored and logged; whether to map it
// to the Intel power table is decided from the logged values.
IOReturn AirportItlwmSkywalkInterface::setPM_MODE(apple80211_pm_mode *input)
{
    if (!input) return kIOReturnBadArgument;
    uint32_t mode;
    memcpy(&mode, reinterpret_cast<const uint8_t *>(input) + 4, sizeof(mode));
    if (mode != airitlPmMode)
        AIRITL_DBG("WCL PM mode: %u -> %u (radio stays CAM)\n", airitlPmMode, mode);
    airitlPmMode = mode;
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::setPOWERSAVE: value at +4 must be < 2,
// else raw 0x66; stores it and returns 0. getPOWERSAVE
// returns the stored value.
IOReturn AirportItlwmSkywalkInterface::setPOWERSAVE(apple80211_powersave_data *pd)
{
    if (!pd) return kIOReturnBadArgument;
    if (pd->powersave_level >= 2) return (IOReturn)0x66;
    __atomic_store_n(&airitlPowersave, pd->powersave_level, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
}

static uint32_t airitlPowersaveLevel() { return __atomic_load_n(&airitlPowersave, __ATOMIC_RELAXED); }

// AppleBCMWLANCore::setWCL_QOS_PARAMS -> setQosParams:
// mask byte +0x16: bit0 retry (+0), bit1 RTS (+4), bit2 VO lifetime (+8),
// bit3 VI lifetime (+0xc), bit4 PM (+0x10). Stored and logged here; mapping
// to Intel retry/EDCA levers is decided from the logged values.
IOReturn AirportItlwmSkywalkInterface::setWCL_QOS_PARAMS(apple80211_wcl_qos_params *input)
{
    if (!input) return (IOReturn)0xe00002bc;
    memcpy(airitlQosParams, input, sizeof(airitlQosParams));
    uint32_t v[5];
    memcpy(v, airitlQosParams, sizeof(v));
    AIRITL_DBG("WCL qos params: mask 0x%02x retry %u rts %u voLife %u viLife %u pm %u\n",
          airitlQosParams[0x16], v[0], v[1], v[2], v[3], v[4]);
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::getWCL_FW_HOT_CHANNELS: count (max 7)
// at +0x10 from firmware `roam_channels_in_hotlist`. Intel firmware keeps no
// such list, so the true answer is zero channels.
IOReturn AirportItlwmSkywalkInterface::getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels *output)
{
    if (!output) return kIOReturnBadArgument;
    memset(output, 0, 0x11);   // 7 u16 entries + count at +0x10 (Apple writes no further)
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::getMAX_NSS_FOR_AP: null -> 0xe00002c2;
// the `btc_nss` reply word goes to +4 (spatial streams allowed under BT coex).
// Ours: the Intel HAL's applied BT-coex SMPS state (ItlIwx.cpp:5299-5313).
IOReturn AirportItlwmSkywalkInterface::getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data *output)
{
    if (!output) return (IOReturn)0xe00002c2;
    if (!fHalService || !fHalService->getDriverInfo()) return kIOReturnNotReady;
    uint32_t nss = (uint32_t)fHalService->getDriverInfo()->getBtCoexMaxNSS();
    memcpy(reinterpret_cast<uint8_t *>(output) + 4, &nss, sizeof(nss));
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::getBEACON_INFO: u32 length at +0, raw
// 802.11 beacon at +4 (max 0x800, else length 0 and 0xe00002db), HE flag at
// +0x804. No beacon available: Apple writes 0xdeadbeef00000000 to the first
// 8 bytes and returns the iovar error. Ours: the last beacon of our BSS that
// the firmware passed to the host (beacon filtering is on, so it can be
// older than Apple's firmware copy).
IOReturn AirportItlwmSkywalkInterface::getBEACON_INFO(apple80211_beacon_info_t *output)
{
    if (!output) return kIOReturnBadArgument;
    if (!fHalService) return kIOReturnNotReady;
    struct ieee80211com *ic = fHalService->get80211Controller();
    uint8_t *out = reinterpret_cast<uint8_t *>(output);
    uint32_t len = 0;
    bool ok = false;
    for (int attempt = 0; ic && attempt < 4 && !ok; ++attempt) {
        uint32_t g0 = __atomic_load_n(&ic->ic_bcn_gen, __ATOMIC_ACQUIRE);
        if (g0 & 1) continue;
        len = ic->ic_bcn_len;
        if (len && len <= sizeof(ic->ic_bcn_frame))
            memcpy(out + 4, ic->ic_bcn_frame, len);
        ok = __atomic_load_n(&ic->ic_bcn_gen, __ATOMIC_ACQUIRE) == g0;
    }
    // Apple's validity test: length >= 0x25 and frame control & 0xfc == 0x80
    // (beacon). The cached frame must belong to the current BSS
    // (addr3 at frame +16), so a beacon from before a roam is never returned.
    const bool valid = ok && len >= 0x25 && len <= 0x800 &&
        ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
        (out[4] & 0xfc) == 0x80 &&
        memcmp(out + 4 + 16, ic->ic_bss->ni_bssid, 6) == 0;
    if (ok && len > 0x800) {
        uint32_t zero = 0;
        memcpy(out, &zero, sizeof(zero));
        return (IOReturn)0xe00002db;   // Apple: frame too large, kIOReturnNoSpace
    }
    if (!valid) {
        static const uint8_t marker[8] = {0, 0, 0, 0, 0xef, 0xbe, 0xad, 0xde};
        memcpy(out, marker, sizeof(marker));
        return kIOReturnNotReady;
    }
    memcpy(out, &len, sizeof(len));
    out[0x804] = (ic->ic_bss && (ic->ic_bss->ni_flags & IEEE80211_NODE_HE)) ? 1 : 0;
    return kIOReturnSuccess;
}
#endif
