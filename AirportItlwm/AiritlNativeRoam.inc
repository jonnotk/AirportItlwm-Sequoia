#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::setWCL_REASSOC(apple80211_reassoc *input)
{
    if (!input) return kIOReturnBadArgument;
    if (!nativeControlReady()) return kIOReturnNotReady;
    apple80211_reassoc request = *input;
    if (!AiritlWcl::validRoamRequest(request)) return kIOReturnBadArgument;
    return _fCommandGate->runAction(nativeRoamGated, this, &request);
}
IOReturn AirportItlwmSkywalkInterface::nativeRoamGated(OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    if (!argument || !airitlInNativeGate()) return kIOReturnBadArgument;
    return self->startNativeRoam(*static_cast<apple80211_reassoc *>(argument));
}
IOReturn AirportItlwmSkywalkInterface::startNativeRoam(const apple80211_reassoc &request)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    if (!AiritlWcl::validRoamRequest(request)) return kIOReturnBadArgument;
    if (fNativeRoam.active || fNativeRoam.draining || fNativeJoinDispatching || fNativeControlOwner ||
        fNativeJoinState.active || fNativeJoinState.draining || fNativeScanState.active || fNativeScanState.draining ||
        fHalService->nativeKeyInstallPending()) return kIOReturnBusy;
    auto *ic = fHalService->get80211Controller();
    if (!instance->power_state || !ic || !ic->ic_native_controlled ||
        !(ic->ic_ac.ac_if.if_flags & IFF_RUNNING) || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss ||
        !fNativeConnectReported || !fNativeBssConfigured || !fNativeBssValid ||
        !ic->ic_native_join_generation || ic->ic_native_join_generation != fNativeJoinState.generation)
        return kIOReturnNotReady;
    if (!fHalService->nativeReassociationReady() || !fHalService->nativeRegulatoryReady() || (ic->ic_flags & IEEE80211_F_BGSCAN) ||
        ic->ic_bss->ni_unref_cb || ic->ic_bss->ni_refcnt != 1) return kIOReturnBusy;
    ItlNativeCurrentRates currentRates = {};
    if (!fHalService->copyNativeCurrentRates(&currentRates)) return kIOReturnBusy;
    const uint32_t allowedAuth = APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK;
    if ((request.flags & ~1U) || current_authtype_lower != APPLE80211_AUTHTYPE_OPEN ||
        (current_authtype_upper & ~allowedAuth) || (ic->ic_bss->ni_flags & IEEE80211_NODE_MFP) ||
        (ic->ic_bss->ni_rsncaps & IEEE80211_RSNCAP_MFPR)) return kIOReturnUnsupported;
    if (request.ssidLength != ic->ic_bss->ni_esslen ||
        memcmp(request.ssid, ic->ic_bss->ni_essid, request.ssidLength)) return kIOReturnUnsupported;
    for (unsigned i = 0; i < request.channelCount; ++i) {
        const unsigned channel = request.channels[i] & 0xff;
        const unsigned band = request.channels[i] & 0xff00;
        if (!channel || (band != 0x1000 && band != 0xd000) || !ic->ic_channels[channel].ic_flags ||
            (band == 0x1000 && !(ic->ic_channels[channel].ic_flags & IEEE80211_CHAN_2GHZ)) ||
            (band == 0xd000 && !(ic->ic_channels[channel].ic_flags & IEEE80211_CHAN_5GHZ))) return kIOReturnUnsupported;
    }
    struct ieee80211_node *selected = NULL, *candidate;
    RB_FOREACH(candidate, ieee80211_tree, &ic->ic_tree) {
        if (candidate == ic->ic_bss || candidate->ni_state == IEEE80211_STA_COLLECT ||
            !candidate->ni_chan || candidate->ni_chan == IEEE80211_CHAN_ANYC ||
            !memcmp(candidate->ni_bssid, ic->ic_bss->ni_bssid, 6) ||
            candidate->ni_esslen != request.ssidLength || memcmp(candidate->ni_essid, request.ssid, request.ssidLength) ||
            !AiritlWcl::roamBssidAllowed(request, candidate->ni_bssid) ||
            (candidate->ni_rsncaps & IEEE80211_RSNCAP_MFPR)) continue;
        const unsigned channel = ieee80211_chan2ieee(ic, candidate->ni_chan);
        if (!channel || channel > 255 || !ic->ic_channels[channel].ic_flags) continue;
        bool channelAllowed = request.channelCount == 0;
        for (unsigned i = 0; i < request.channelCount; ++i)
            channelAllowed |= (request.channels[i] & 0xff) == channel &&
                (((request.channels[i] & 0xff00) == 0x1000 && (candidate->ni_chan->ic_flags & IEEE80211_CHAN_2GHZ)) ||
                 ((request.channels[i] & 0xff00) == 0xd000 && (candidate->ni_chan->ic_flags & IEEE80211_CHAN_5GHZ)));
        if (!channelAllowed) continue;
        if (ic->ic_flags & IEEE80211_F_RSNON) {
            if (!(candidate->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) ||
                !(candidate->ni_rsnprotos & ic->ic_bss->ni_rsnprotos) ||
                !(candidate->ni_rsnakms & ic->ic_bss->ni_rsnakms) ||
                !(candidate->ni_rsnciphers & ic->ic_bss->ni_rsncipher) ||
                candidate->ni_rsngroupcipher != ic->ic_bss->ni_rsngroupcipher ||
                (candidate->ni_rsncaps & 0x3c) != (ic->ic_bss->ni_rsncaps & 0x3c)) continue;
        } else if (candidate->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) continue;
        if (!selected || candidate->ni_rssi > selected->ni_rssi) selected = candidate;
    }
    if (!selected) return kIOReturnNotFound;
    AiritlWcl::RequestState next = fNativeJoinState;
    const uint64_t token = next.begin();
    if (!token) return kIOReturnBusy;
    ieee80211_ref_node(selected);
    if (!ieee80211_prepare_native_reassoc(ic, selected, ic->ic_native_join_generation, token)) {
        ieee80211_release_node(ic, selected); return kIOReturnUnsupported;
    }
    const uint32_t protocols = ic->ic_bss->ni_rsnprotos, akms = ic->ic_bss->ni_rsnakms;
    const auto cipher = ic->ic_bss->ni_rsncipher;
    const auto group = ic->ic_bss->ni_rsngroupcipher;
    AiritlNativeRoamState operation = {};
    operation.request = request; operation.generation = token; operation.active = true;
    memcpy(operation.origin, ic->ic_bss->ni_bssid, 6); memcpy(operation.target, selected->ni_bssid, 6);
    clock_interval_to_deadline(10, kSecondScale, &operation.deadline);
    const int16_t targetRssi = int16_t(int(selected->ni_rssi) + IWM_MIN_DBM);
    const uint64_t coreGeneration = ic->ic_native_core_generation;
    auto *originBss = ic->ic_bss;
    // Pure preflight succeeded. Commit without a callback or yield, then fence
    // all new data dispatch before replacing the old BSS object.
    fNativeJoinState = next; ic->ic_native_join_generation = token; fNativeRoam = operation;
    ic->ic_xflags |= IEEE80211_F_TX_MGMT_ONLY;
    ic->ic_bss->ni_port_valid = 0;
    ic->ic_bss->ni_flags &= ~(IEEE80211_NODE_TXPROT | IEEE80211_NODE_RXPROT);
    fNativeConnectReported = false;
    ieee80211_clear_native_assoc_ie(ic);
    memcpy(fNativeJoinBssid, operation.target, 6);
    memcpy(ic->ic_des_bssid, operation.target, 6); ic->ic_flags |= IEEE80211_F_DESBSSID;
    ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
    ic->ic_rsnprotos = protocols; ic->ic_rsnakms = akms;
    ic->ic_rsnciphers = cipher; ic->ic_rsngroupcipher = group;
    clearNativeBss();
    if (!fNativeRoam.active || fNativeRoam.generation != token || fNativeJoinState.generation != token) {
        ieee80211_release_node(ic, selected); return kIOReturnSuccess;
    }
    uint8_t start[12] = {}; AiritlWcl::write32(start + 4, 0xe3ff8100);
    uint8_t prep[12] = {}; AiritlWcl::write32(prep, 0xffffffff);
    memcpy(prep + 4, operation.target, 6); AiritlWcl::write16(prep + 10, uint16_t(targetRssi));
    postMessage(0x89, start, sizeof(start), true);
    if (!fNativeRoam.active || fNativeRoam.generation != token || fNativeJoinState.generation != token) {
        ieee80211_release_node(ic, selected); return kIOReturnSuccess;
    }
    postMessage(0x8b, prep, sizeof(prep), true);
    if (fNativeRoam.active && fNativeRoam.generation == token && fNativeJoinState.generation == token) {
        if (!nativeControlReady() || fHalService->get80211Controller() != ic ||
            ic->ic_native_core_generation != coreGeneration || ic->ic_native_join_generation != token ||
            ic->ic_bss != originBss || !(ic->ic_ac.ac_if.if_flags & IFF_RUNNING) ||
            ic->ic_bss->ni_refcnt != 1 || ic->ic_bss->ni_unref_cb ||
            selected->ni_state == IEEE80211_STA_COLLECT || fHalService->nativeKeyInstallPending())
            finishNativeRoam(kIOReturnNotReady, IEEE80211_REASON_AUTH_LEAVE);
        else ieee80211_node_join_bss(ic, selected, 1);
    }
    ieee80211_release_node(ic, selected);
    return kIOReturnSuccess;
}
void AirportItlwmSkywalkInterface::handleNativeRoamEvent(int event, int status, uint64_t token)
{
    if (!fNativeRoam.active || fNativeRoam.generation != token || fNativeJoinState.generation != token) return;
    if (event == 1 && !fNativeRoam.associationReported) {
        uint8_t response[8] = {};
        AiritlWcl::write32(response, status ? uint32_t(kIOReturnError) : 0);
        AiritlWcl::write32(response + 4, status && status <= 45 ? 0xe0823000U | uint32_t(status) : status ? 0xe3ff8100U : 0);
        fNativeRoam.associationReported = true;
        postMessage(0x49, response, sizeof(response), true);
        if (!fNativeRoam.active || fNativeRoam.generation != token || fNativeJoinState.generation != token) return;
        if (status) finishNativeRoam(kIOReturnError, IEEE80211_REASON_AUTH_LEAVE);
        else pollNativeRoam();
    } else if (event == 2) pollNativeRoam();
    else if (event == 3 || event == 5) finishNativeRoam(kIOReturnError, status);
    else if (event == 4) finishNativeRoam(kIOReturnError, IEEE80211_REASON_AUTH_LEAVE);
}
void AirportItlwmSkywalkInterface::pollNativeRoam()
{
    if (!fNativeRoam.active || fNativeJoinDispatching) return;
    const uint64_t token = fNativeRoam.generation;
    if (fNativeJoinState.generation != token || !fNativeJoinState.active) return;
    uint64_t now = 0; clock_get_uptime(&now);
    if (now >= fNativeRoam.deadline) { finishNativeRoam(kIOReturnTimeout, IEEE80211_REASON_AUTH_LEAVE); return; }
    if (!fNativeRoam.associationReported || !fNativeRoam.firstBeacon ||
        !nativeJoinCompletionMatches(token, fNativeRoam.target)) return;
    uint8_t target[6]; memcpy(target, fNativeRoam.target, 6);
    fNativeJoinDispatching = true;
    if (!fNativeLinkReported) {
        uint8_t link[12];
        AiritlWcl::encodeLinkStatus(link, target, true, IO80211InfraInterface::isDebounceOnGoing());
        fNativeLinkReported = true;
        postMessage(0xd8, link, sizeof(link), true);
    }
    fNativeJoinDispatching = false;
    if (!fNativeRoam.active || fNativeRoam.generation != token ||
        !nativeJoinCompletionMatches(token, target) || !fNativeLinkReported || !fNativeBssConfigured) return;
    uint8_t completion[168]; AiritlWcl::encodeRoamStatus(completion, 0);
    // Retire only this operation before its terminal callback can replace it.
    fNativeRoam.active = false;
    fNativeConnectReported = true;
    fNativeJoinState.finish(token);
    if (fHalService && fHalService->get80211Controller())
        ieee80211_clear_native_reassoc(fHalService->get80211Controller());
    postMessage(0x50, completion, sizeof(completion), true);
}
void AirportItlwmSkywalkInterface::finishNativeRoam(IOReturn status, int reason)
{
    if (!fNativeRoam.active) return;
    const uint64_t token = fNativeRoam.generation;
    uint8_t target[6]; memcpy(target, fNativeRoam.target, 6);
    fNativeRoam.active = false; fNativeRoam.draining = true;
    fNativeRoam.hardwareQuiesced = false; fNativeRoam.downAcknowledged = false;
    fNativeJoinState.cancel();
    const uint64_t terminalGeneration = fNativeJoinState.generation;
    fNativeConnectReported = false;
    if (fHalService && fHalService->get80211Controller()) {
        auto *ic = fHalService->get80211Controller();
        if (ic->ic_native_join_generation == token) {
            ic->ic_xflags |= IEEE80211_F_TX_MGMT_ONLY;
            if (ic->ic_bss) {
                ic->ic_bss->ni_port_valid = 0;
                ic->ic_bss->ni_flags &= ~(IEEE80211_NODE_TXPROT | IEEE80211_NODE_RXPROT);
            }
            ieee80211_clear_native_assoc_ie(ic);
        }
        ieee80211_clear_native_reassoc(ic);
    }
    clearNativeBss();
    if (!fNativeRoam.draining || fNativeRoam.generation != token || fNativeJoinState.generation != terminalGeneration) return;
    uint8_t completion[168]; AiritlWcl::encodeRoamStatus(completion, status ? uint32_t(status) : uint32_t(kIOReturnError));
    postMessage(0x50, completion, sizeof(completion), true);
    if (!fNativeRoam.draining || fNativeRoam.generation != token || fNativeJoinState.generation != terminalGeneration) return;
    // Native Net's current-BSS deauth branch accepts the real target address
    // even while its current BSS still names the lost origin association.
    uint8_t deauth[20] = {};
    AiritlWcl::write16(deauth + 8, uint16_t(reason > 0 ? reason : IEEE80211_REASON_AUTH_LEAVE));
    memcpy(deauth + 10, target, 6); deauth[16] = 1;
    postMessage(0xcb, deauth, sizeof(deauth), true);
    // CB drives native leave first. Disabling control here would race that
    // queued leave/down chain while the HAL stop yields the shared gate.
}
void AirportItlwmSkywalkInterface::cancelNativeRoam()
{
    if (!fNativeRoam.active) return;
    fNativeRoam.active = false;
    // The caller owns reset/leave and its native terminal notification.
    fNativeRoam.draining = false;
    if (fNativeJoinState.active) fNativeJoinState.cancel();
    if (fHalService && fHalService->get80211Controller())
        ieee80211_clear_native_reassoc(fHalService->get80211Controller());
}
void AirportItlwmSkywalkInterface::acknowledgeNativeRoamDown(const uint8_t *bssid)
{
    uint8_t any = 0; for (unsigned i = 0; i < 6; ++i) any |= bssid[i];
    // Native updateLinkState zeroes the address on down. No new operation may
    // acquire this state while the terminal drain awaits that acknowledgement.
    if (!fNativeRoam.draining || fNativeRoam.active || fNativeJoinState.active ||
        (any && memcmp(bssid, fNativeRoam.target, 6) && memcmp(bssid, fNativeRoam.origin, 6))) return;
    fNativeRoam.downAcknowledged = true;
    if (fNativeRoam.hardwareQuiesced) fNativeRoam.draining = false;
    else if (instance) instance->abortNativeJoin();
}
#endif
