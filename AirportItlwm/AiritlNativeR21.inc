#if __IO80211_TARGET >= __MAC_15_0
// WCL requests whose Apple driver behaviour is matched here.

// Seven 64-bit slots, 0x38 bytes, cumulative; WCL takes deltas.
// Producer AppleBCMWLANCore::getWCL_TRAFFIC_COUNTERS, consumer
// WCLNetManager::checkTrafficCounters / loadTrafficCounters (IO80211Family):
//   [0] TX AC2+AC3  [1] RX AC2+AC3 (getRealTimeTrafficClassRxCounter)
//   [2] TX all ACs  [3] RX all ACs (dumpWmeCounters "per AC counters: Tx ... Rx counters ...")
//   [4] [5] NAN interface, none here  [6] uptime in nanoseconds.
// realTimeNumPackets = [0]+[1]+[4]+[5]; infraNumPackets = [2]+[3].
IOReturn AirportItlwmSkywalkInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *output)
{
    if (!output) return kIOReturnBadArgument;
    if (!fHalService) return kIOReturnNotReady;
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic) return kIOReturnNotReady;
    uint64_t tx[EDCA_NUM_AC], rx[EDCA_NUM_AC];
    for (unsigned i = 0; i < EDCA_NUM_AC; ++i) {
        // Aligned 64-bit loads; each counter has a single writer context.
        tx[i] = __atomic_load_n(&ic->ic_wcl_tx_ac[i], __ATOMIC_RELAXED);
        rx[i] = __atomic_load_n(&ic->ic_wcl_rx_ac[i], __ATOMIC_RELAXED);
    }
    uint64_t now, ns;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &ns);
    uint64_t slots[7] = {
        tx[EDCA_AC_VI] + tx[EDCA_AC_VO],
        rx[EDCA_AC_VI] + rx[EDCA_AC_VO],
        tx[0] + tx[1] + tx[2] + tx[3],
        rx[0] + rx[1] + rx[2] + rx[3],
        0,
        0,
        ns,
    };
    memcpy(output, slots, sizeof(slots));
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::setBTCOEX_PROFILE accepts a
// 0x38-byte profile when version (u16 @0) is 1..3, byte[3] < 5 and the slot
// byte[4] < 10, stores it per slot, else returns 0xe00002c2. No Intel firmware
// command takes these profiles; store and log only.
static uint8_t airitlBtcoexProfiles[10][0x38];
IOReturn AirportItlwmSkywalkInterface::setBTCOEX_PROFILE(apple80211_btcoex_profile *input)
{
    if (!input) return kIOReturnBadArgument;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(input);
    uint16_t version;
    memcpy(&version, p, sizeof(version));
    // Apple returns 0xe00002c2 for an invalid profile.
    if (version < 1 || version > 3 || p[3] >= 5 || p[4] >= 10) return (IOReturn)0xe00002c2;
    memcpy(airitlBtcoexProfiles[p[4]], p, 0x38);
    AIRITL_DBG("WCL btcoex profile: version %u slot %u mode %u bytes %02x%02x%02x%02x%02x%02x%02x%02x\n",
          version, p[4], p[3], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    return kIOReturnSuccess;
}

// AppleBCMWLANCore::setWCL_SET_SCAN_HOME_AWAY_TIME
// passes the first u32 to ScanAdapter::setScanHomeAwayTime
// (iovar scan_home_away_time). Log only; the cap is chosen from the logged values.
IOReturn AirportItlwmSkywalkInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *input)
{
    if (!input) return kIOReturnBadArgument;
    uint32_t value;
    memcpy(&value, input, sizeof(value));
    AIRITL_DBG("WCL scan home/away time: %u\n", value);
    return kIOReturnSuccess;
}

// Informational requests: Apple stores them and returns 0
// (setOS_FEATURE_FLAGS, setINFRA_ENUMERATED,
// setUSB_HOST_NOTIFICATION, setWCL_LINK_UP_DONE). Nothing here acts on them.
IOReturn AirportItlwmSkywalkInterface::setOS_FEATURE_FLAGS(apple80211_feature_flags *input)
{
    return input ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setINFRA_ENUMERATED(apple80211_infra_enumerated *input)
{
    return input ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data *input)
{
    return input ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LINK_UP_DONE(void *)
{
    return kIOReturnSuccess;
}
#endif
