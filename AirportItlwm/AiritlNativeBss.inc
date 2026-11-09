#if __IO80211_TARGET >= __MAC_15_0
#include <HAL/ItlNativeCurrentRates.hpp>

void AirportItlwmSkywalkInterface::clearNativeBss()
{
    bzero(&fNativeBss, sizeof(fNativeBss));
    fNativeBssValid = false;
    fNativeBssConfigured = false;
    fNativeBssGeneration = 0;
    fNativeLinkReported = false;
    fNativeFirstBeaconReported = false;
    if (fHalService && fHalService->get80211Controller())
        fHalService->get80211Controller()->ic_native_fastlane_override = false;
    if (nativeControlReady()) IOSkywalkNetworkInterface::setFastlaneCapable(false);
}

bool AirportItlwmSkywalkInterface::nativeJoinCompletionMatches(uint64_t token, const uint8_t *bssid) const
{
    if (!nativeControlReady() || !fNativeJoinState.active || (!fNativeJoin && !fNativeRoam.active) ||
        fNativeJoinState.generation != token || !fNativeBssValid ||
        fNativeBssGeneration != token || memcmp(fNativeJoinBssid, bssid, 6)) return false;
    auto *ic = fHalService->get80211Controller();
    return ic && (ic->ic_ac.ac_if.if_flags & IFF_RUNNING) &&
        ic->ic_native_join_generation == token && ic->ic_state == IEEE80211_S_RUN &&
        ic->ic_bss && !memcmp(ic->ic_bss->ni_bssid, bssid, 6) &&
        !memcmp(fNativeBss.bytes + 0x29, bssid, 6) &&
        (!(ic->ic_flags & IEEE80211_F_RSNON) || ic->ic_bss->ni_port_valid);
}

IOReturn AirportItlwmSkywalkInterface::getWCL_BSS_INFO(apple80211_beacon_msg *output)
{
    if (!output) return kIOReturnError;
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeBssGated, this, output, NULL);
}
IOReturn AirportItlwmSkywalkInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *output)
{
    if (!output) return kIOReturnError;
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeBssGated, this, output, reinterpret_cast<void *>(1));
}
IOReturn AirportItlwmSkywalkInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *input)
{
    if (!input) return kIOReturnError;
    if (!nativeControlReady()) return kIOReturnNotReady;
    apple80211_fastlane request = *input;
    return _fCommandGate->runAction(nativeBssGated, this, &request, reinterpret_cast<void *>(2));
}
IOReturn AirportItlwmSkywalkInterface::nativeBssGated(OSObject *owner, void *object, void *argument, void *operation, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    if (!argument || !airitlInNativeGate()) return kIOReturnBadArgument;
    auto *ic = self->fHalService->get80211Controller();
    if (!ic) return kIOReturnNotAttached;
    const uintptr_t op = reinterpret_cast<uintptr_t>(operation);
    if (op == 2) {
        const uint64_t token = self->fNativeJoinState.generation;
        uint8_t bssid[6]; memcpy(bssid, self->fNativeJoinBssid, sizeof(bssid));
        const auto *request = static_cast<apple80211_fastlane *>(argument);
        ic->ic_native_fastlane_override = request->capable && request->overrideAdmissionControl;
        self->IOSkywalkNetworkInterface::setFastlaneCapable(request->capable != 0);
        // This is the last required driver call in native setCurrentBSS.
        // D8 is queued, so its dispatch alone cannot acknowledge completion.
        if (self->fNativeLinkReported && self->nativeJoinCompletionMatches(token, bssid)) {
            self->fNativeBssConfigured = true;
            self->pollNativeJoin();
        }
        return kIOReturnSuccess;
    }
    if (op > 1) return kIOReturnBadArgument;
    if (!(ic->ic_ac.ac_if.if_flags & IFF_RUNNING) || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss || !self->fNativeBssValid ||
        self->fNativeBssGeneration != ic->ic_native_join_generation ||
        self->fNativeBssGeneration != self->fNativeJoinState.generation ||
        memcmp(self->fNativeBss.bytes + 0x29, ic->ic_bss->ni_bssid, 6)) return kIOReturnNotReady;
    if (op == 0) {
        if (ic->ic_bss->ni_esslen > 32) return kIOReturnBadArgument;
        auto *output = static_cast<apple80211_beacon_msg *>(argument);
        memcpy(output, &self->fNativeBss, sizeof(*output));
        // Firmware BSS info carries the associated SSID even for hidden beacons.
        // The original observed information elements remain unchanged.
        bzero(output->bytes + 6, 32);
        memcpy(output->bytes + 6, ic->ic_bss->ni_essid, ic->ic_bss->ni_esslen);
        output->bytes[0x26] = ic->ic_bss->ni_esslen;
        return kIOReturnSuccess;
    }
    ItlNativeCurrentRates rates = {};
    if (!self->fHalService->copyNativeCurrentRates(&rates) ||
        !ic->ic_native_assoc_ie_valid ||
        ic->ic_native_assoc_ie_generation != ic->ic_native_join_generation ||
        memcmp(ic->ic_native_assoc_ie_bssid, ic->ic_bss->ni_bssid, 6)) return kIOReturnNotReady;
    apple80211_extended_bss_info value;
    if (!AiritlWcl::encodeExtendedBss(rates, ic->ic_native_assoc_ie,
        ic->ic_native_assoc_ie_len, value)) return kIOReturnBadArgument;
    memcpy(argument, &value, sizeof(value));
    return kIOReturnSuccess;
}
#endif
