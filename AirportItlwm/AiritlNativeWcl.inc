#if __IO80211_TARGET >= __MAC_15_0
#include <HAL/ItlNativeScanProfile.hpp>
#ifndef AIRITL_ENABLE_BSDIF
#include "IOPCIEDeviceWrapper.hpp"
#endif
extern IOCommandGate *_fCommandGate;
extern IO80211WorkQueue *_fWorkloop;

struct AiritlNativeCompletion { int status; uint64_t token; int event; };
struct AiritlNativeBeaconCall { const uint8_t *frame; size_t length; const AiritlWcl::BeaconInput *info; };
static bool airitlInNativeGate()
{
    return _fCommandGate && _fCommandGate->getWorkLoop() && _fCommandGate->getWorkLoop()->inGate();
}
IOReturn AirportItlwmSkywalkInterface::nativeEventGated(OSObject *owner, void *object, void *operation, void *argument, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    const uintptr_t op = reinterpret_cast<uintptr_t>(operation);
    auto *completion = static_cast<AiritlNativeCompletion *>(argument);
    switch (op) {
        case 1: self->handleHalScanComplete(completion->status, completion->token); break;
        case 2: self->handleHalJoinEvent(completion->event, completion->status, completion->token); break;
        case 3: {
            auto *beacon = static_cast<AiritlNativeBeaconCall *>(argument);
            return self->publishNativeBeacon(beacon->frame, beacon->length, *beacon->info) ? kIOReturnSuccess : kIOReturnNotFound;
        }
        case 4: self->pollNativeJoin(); break;
        case 5: self->cancelNativeRequests(*static_cast<IOReturn *>(argument)); break;
        case 6: self->nativeHalQuiesced(); break;
        case 7: self->pollNativeScan(); break;
        default: return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::nativeControlReady() const
{
    return instance && !instance->fStopping && fHalService && _fCommandGate && fHalService->get80211Controller() &&
           fHalService->supportsNativeScan() && fNativeObserverReady && !fNativeResetPending;
}
#ifndef AIRITL_ENABLE_BSDIF
bool AirportItlwmSkywalkInterface::nativeLinkDownReady() const
{
    // The caller holds the original main gate before reading this graph.
    if (!instance || !instance->fControllerTimerLock) return false;
    IOLockLock(instance->fControllerTimerLock);
    // Admission point: cleanup can close later, but must wait for this main-gate frame.
    const bool admissionOpen = !instance->fStartInProgress && !instance->fCleanupInProgress &&
                               !instance->fStopping && !instance->fControllerTimerClosing;
    IOLockUnlock(instance->fControllerTimerLock);
    if (!admissionOpen) return false;
    if (!nativeTransportAvailable() || fNativeStartPhase != NativeStartPhase::Succeeded ||
        fNativeStopPhase != NativeStopPhase::NotCalled || fStopThread || fDatapathRetiring)
        return false;
    if (!instance->fNativeInitComplete ||
        instance->fNativeStartPhase != AirportItlwm::NativeStartPhase::Succeeded ||
        !instance->fControllerStarted || !instance->fHalAttached ||
        !instance->fCommandGateAcquired || !instance->fCommandGateAdded ||
        instance->fNetIf != this || !fHalService || fHalService != instance->fHalService)
        return false;
    auto *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_if.controller != instance || getProvider() != instance ||
        !instance->fWrapperProvider || instance->getProvider() != instance->fWrapperProvider)
        return false;
    // HAL init binds this controller, loop and gate once, before the successful attach.
    return _fCommandGate && _fWorkloop && fDatapathWorkQueue == _fWorkloop &&
           instance->fWrapperProvider->getWorkLoop() == _fWorkloop &&
           _fCommandGate->getWorkLoop() == _fWorkloop;
}
#endif
void AirportItlwmSkywalkInterface::releaseNativeScan()
{
    if (fNativeScan) { bzero(fNativeScan, sizeof(*fNativeScan)); IOFree(fNativeScan, sizeof(*fNativeScan)); fNativeScan = NULL; }
    bzero(&fNativeScanProfile, sizeof(fNativeScanProfile));
}
void AirportItlwmSkywalkInterface::releaseNativeJoin()
{
    if (fHalService && fHalService->get80211Controller())
        ieee80211_clear_native_reassoc(fHalService->get80211Controller());
    if (fNativeJoin) { bzero(fNativeJoin, sizeof(*fNativeJoin)); IOFree(fNativeJoin, sizeof(*fNativeJoin)); fNativeJoin = NULL; }
}
IOReturn AirportItlwmSkywalkInterface::nativeScanGated(OSObject *owner, void *object, void *arg, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    return self && self->instance == owner ? self->startNativeScan(static_cast<apple80211ScanRequest *>(arg)) : kIOReturnNotAttached;
}
IOReturn AirportItlwmSkywalkInterface::nativeJoinGated(OSObject *owner, void *object, void *arg, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    return self && self->instance == owner ? self->startNativeJoin(static_cast<apple80211AssocCandidates *>(arg)) : kIOReturnNotAttached;
}
IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_REQ(apple80211ScanRequest *input)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    auto *copy = static_cast<apple80211ScanRequest *>(IOMalloc(sizeof(*input)));
    if (!copy) return kIOReturnNoMemory;
    if (AiritlWcl::copyScan(input, sizeof(*input), *copy) != AiritlWcl::Error::none) {
        bzero(copy, sizeof(*copy)); IOFree(copy, sizeof(*copy)); return kIOReturnBadArgument;
    }
    IOReturn result = _fCommandGate->runAction(nativeScanGated, this, copy);
    // The gated method takes ownership only after it accepts the request.
    if (result != kIOReturnSuccess) { bzero(copy, sizeof(*copy)); IOFree(copy, sizeof(*copy)); }
    return result;
}
IOReturn AirportItlwmSkywalkInterface::startNativeScan(apple80211ScanRequest *request)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    if (!fHalService->nativeRegulatoryReady()) return kIOReturnBusy;
    if (fNativeScanState.active || fNativeScanState.draining || fNativeJoinState.active || fNativeJoinState.draining || fNativeJoinDispatching || fNativeControlOwner || fNativeRoam.draining)
        return kIOReturnBusy;
    auto *ic = fHalService->get80211Controller();
    if (!instance->power_state) return kIOReturnNotReady;
    if (request->mode > 2 || request->shortSsid || (request->flags & 3)) return kIOReturnUnsupported;
    ItlNativeScanProfile profile = {};
    profile.passive = request->mode == 2;
    profile.background = ic->ic_state == IEEE80211_S_RUN;
    if (!itlNativeScanDwellMillisecondsToTU(request->activeDwell, profile.activeDwell) ||
        !itlNativeScanDwellMillisecondsToTU(request->passiveDwell, profile.passiveDwell) ||
        (profile.background && !itlNativeScanDwellMillisecondsToTU(request->homeDwell, profile.homeDwell)))
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < request->channelCount; ++i) {
        const uint32_t channel = request->channels[i].channel;
        if (!ic->ic_channels[channel].ic_flags) return kIOReturnUnsupported;
        const uint32_t band = request->channels[i].flags & 0x18;
        if ((band == 8 && !(ic->ic_channels[channel].ic_flags & IEEE80211_CHAN_2GHZ)) ||
            (band == 0x10 && !(ic->ic_channels[channel].ic_flags & IEEE80211_CHAN_5GHZ))) return kIOReturnUnsupported;
        profile.channelBitmap[channel / 8] |= uint8_t(1U << (channel % 8));
    }
    uint8_t anyBssid = 0, allBssid = 0xff;
    for (unsigned i = 0; i < 6; ++i) { anyBssid |= request->bssid[i]; allBssid &= request->bssid[i]; }
    if (anyBssid && allBssid != 0xff) {
        if (request->bssid[0] & 1) return kIOReturnBadArgument;
        profile.hasBSSID = true; memcpy(profile.bssid, request->bssid, 6);
    }
    if (request->ssidCount) {
        profile.ssidCount = request->ssidCount;
        for (unsigned i = 0; i < profile.ssidCount; ++i) {
            profile.ssids[i].len = request->ssids[i].length;
            memcpy(profile.ssids[i].bytes, request->ssids[i].bytes, profile.ssids[i].len);
        }
    } else if (request->ssidLength) {
        profile.ssidCount = 1; profile.ssids[0].len = request->ssidLength;
        memcpy(profile.ssids[0].bytes, request->ssid, request->ssidLength);
    }
    if (request->privateScan) {
        profile.hasScanMac = true;
        arc4random_buf(profile.scanMac, sizeof(profile.scanMac));
        profile.scanMac[0] = (profile.scanMac[0] & 0xfc) | 2;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(20, kSecondScale, &deadline);
    profile.token = fNativeScanState.beginQueued(deadline);
    fNativeScanProfile = profile;
    fNativeScan = request;
    // Acceptance owns a bounded queued operation. Only firmware completion can
    // report success. Initial asynchronous HAL startup/UID occupancy is retried.
    pollNativeScan();
    return kIOReturnSuccess;
}
void AirportItlwmSkywalkInterface::pollNativeScan()
{
    if (!airitlInNativeGate()) {
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(7));
        return;
    }
    if (!fNativeScanState.active || fNativeScanState.submitted || !fNativeScan) return;
    const uint64_t token = fNativeScanState.generation;
    uint64_t now = 0; clock_get_uptime(&now);
    if (fNativeScanState.queueExpired(now)) { handleHalScanComplete(kIOReturnTimeout, token); return; }
    if (!instance || !instance->power_state) { handleHalScanComplete(kIOReturnAborted, token); return; }
    if (!nativeControlReady()) return;
    auto *ic = fHalService->get80211Controller();
    if (!ic || !fHalService->nativeScanInitializationReady()) return;
    fNativeScanState.markSubmitted(token);
    IOReturn result = fHalService->startNativeScan(fNativeScanProfile, nativeScanCompleted, this);
    if (result == kIOReturnBusy || result == kIOReturnNotReady) fNativeScanState.retryQueued(token);
    else if (result != kIOReturnSuccess) handleHalScanComplete(result, token);
}

void AirportItlwmSkywalkInterface::nativeScanCompleted(void *context, uint64_t token, int32_t status, uint8_t)
{
    auto *self = static_cast<AirportItlwmSkywalkInterface *>(context);
    if (self) self->handleHalScanComplete(status, token);
}
void AirportItlwmSkywalkInterface::handleHalScanComplete(int status, uint64_t token)
{
    if (!airitlInNativeGate()) {
        AiritlNativeCompletion call = {status, token, 0};
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(1), &call);
        return;
    }
    if (!fNativeScanState.finish(token)) return;
    releaseNativeScan();
    uint32_t nativeStatus = AiritlWcl::scanStatus(status == kIOReturnSuccess, status == kIOReturnAborted, status == kIOReturnTimeout);
    postMessage(0xed, &nativeStatus, sizeof(nativeStatus), true);
}
IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_ABORT(void *)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeAbortGated, this, NULL);
}
IOReturn AirportItlwmSkywalkInterface::nativeAbortGated(OSObject *owner, void *object, void *join, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner || !self->nativeControlReady()) return kIOReturnNotAttached;
    if (!join) {
        if (!self->fNativeScanState.active) return kIOReturnNotFound;
        if (!self->fNativeScanState.submitted) {
            self->handleHalScanComplete(kIOReturnAborted, self->fNativeScanState.generation);
            return kIOReturnSuccess;
        }
        const uint64_t token = self->fNativeScanState.generation;
        if (!self->fNativeScanState.requestCancel(token)) return kIOReturnBusy;
        IOReturn result = self->fHalService->cancelNativeScan(token);
        if (result != kIOReturnSuccess) self->fNativeScanState.cancelRejected(token);
        return result;
    }
    if (self->fNativeRoam.active) {
        self->finishNativeRoam(kIOReturnAborted, IEEE80211_REASON_AUTH_LEAVE);
        return kIOReturnSuccess;
    }
    if (!self->fNativeJoinState.active) return kIOReturnNotFound;
    self->fNativeJoinAbortPending = true;
    self->fNativeJoinState.cancel();
    self->releaseNativeJoin();
    return self->instance->abortNativeJoin();
}

bool AirportItlwmSkywalkInterface::publishNativeBeacon(const uint8_t *frame, size_t length,
                                                       const AiritlWcl::BeaconInput &info)
{
    if (!airitlInNativeGate()) {
        AiritlNativeBeaconCall call = {frame, length, &info};
        return _fCommandGate && _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(3), &call) == kIOReturnSuccess;
    }
    if (!frame || length < 36 || (!fNativeScanState.active && !fNativeJoinState.active && !fNativeConnectReported)) return false;
    auto *message = static_cast<uint8_t *>(IOMalloc(0x840));
    if (!message) return false;
    size_t written = 0;
    if (!AiritlWcl::encodeBeacon(frame, length, info, message, 0x840, written)) {
        IOFree(message, 0x840); return false;
    }
    const bool currentBeacon = (fNativeJoinState.active || fNativeConnectReported) &&
        !memcmp(frame + 16, fNativeJoinBssid, 6);
    if (currentBeacon) {
        bzero(&fNativeBss, sizeof(fNativeBss));
        memcpy(fNativeBss.bytes, message, written);
        fNativeBssGeneration = fNativeJoinState.generation;
        fNativeBssValid = true;
    }
    if (fNativeRoam.active && fNativeRoam.associationReported && !fNativeRoam.firstBeacon &&
        frame[0] == 0x80 && currentBeacon) {
        fNativeRoam.firstBeacon = true;
        pollNativeRoam();
    }
    if (fNativeJoin && fNativeJoinState.active && fNativeAssocReported && !fNativeFirstBeaconReported &&
        frame[0] == 0x80 && !memcmp(frame + 16, fNativeJoinBssid, 6)) {
        const uint64_t token = fNativeJoinState.generation;
        uint8_t bssid[6]; memcpy(bssid, fNativeJoinBssid, sizeof(bssid));
        uint8_t firstBeacon[20]; AiritlWcl::encodeFirstBeacon(firstBeacon, bssid);
        fNativeFirstBeaconReported = true;
        const bool dispatching = fNativeJoinDispatching;
        fNativeJoinDispatching = true;
        postMessage(0xd4, firstBeacon, sizeof(firstBeacon), true);
        fNativeJoinDispatching = dispatching;
        if (fNativeJoinState.generation == token && fNativeJoinState.active && fNativeJoin &&
            !memcmp(fNativeJoinBssid, bssid, sizeof(bssid))) pollNativeJoin();
    }
    bool selected = false;
    if (fNativeScanState.active && fNativeScanState.submitted && !fNativeScanState.cancelling && fNativeScan) {
        selected = !fNativeScan->channelCount;
        for (uint32_t i = 0; i < fNativeScan->channelCount; ++i)
            selected |= fNativeScan->channels[i].channel == info.channel;
        uint8_t any = 0, all = 0xff;
        for (unsigned i = 0; i < 6; ++i) { any |= fNativeScan->bssid[i]; all &= fNativeScan->bssid[i]; }
        if (any && all != 0xff && memcmp(frame + 16, fNativeScan->bssid, 6)) selected = false;
        if (selected) {
            selected = !fNativeScan->ssidCount && !fNativeScan->ssidLength;
            if (fNativeScan->ssidCount) {
                for (uint32_t i = 0; i < fNativeScan->ssidCount; ++i)
                    selected |= !fNativeScan->ssids[i].length ||
                        (message[0x26] == fNativeScan->ssids[i].length &&
                         !memcmp(message + 6, fNativeScan->ssids[i].bytes, message[0x26]));
            } else if (fNativeScan->ssidLength) {
                selected = message[0x26] == fNativeScan->ssidLength &&
                    !memcmp(message + 6, fNativeScan->ssid, message[0x26]);
            }
        }
        if (selected) postMessage(0xc9, message, written, true);
    }
    IOFree(message, 0x840);
    return selected || currentBeacon;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *input)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    auto *copy = static_cast<apple80211AssocCandidates *>(IOMalloc(sizeof(*input)));
    if (!copy) return kIOReturnNoMemory;
    if (AiritlWcl::copyJoin(input, sizeof(*input), *copy) != AiritlWcl::Error::none) {
        bzero(copy, sizeof(*copy)); IOFree(copy, sizeof(*copy)); return kIOReturnBadArgument;
    }
    IOReturn result = _fCommandGate->runAction(nativeJoinGated, this, copy);
    if (result != kIOReturnSuccess) { bzero(copy, sizeof(*copy)); IOFree(copy, sizeof(*copy)); }
    return result;
}
IOReturn AirportItlwmSkywalkInterface::startNativeJoin(apple80211AssocCandidates *request)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    // Apple's MAC agent can return cached-address success without our setter.
    IOReturn readiness = waitForNativeReady();
    if (readiness != kIOReturnSuccess) return readiness;
    if (!fHalService->nativeRegulatoryReady()) return kIOReturnBusy;
    if (fNativeScanState.active || fNativeScanState.draining || fNativeJoinState.active || fNativeJoinState.draining || fNativeJoinDispatching || fNativeControlOwner || fNativeRoam.draining)
        return kIOReturnBusy;
    if (fHalService->nativeKeyInstallPending()) return kIOReturnBusy;
    auto *ic = fHalService->get80211Controller();
    if (!instance->power_state || !(ic->ic_ac.ac_if.if_flags & IFF_RUNNING) || !ic->ic_bss) return kIOReturnNotReady;
    if ((request->flags & 0x40) || (request->apMode != 2 && request->apMode != 3) || request->lowerAuth != APPLE80211_AUTHTYPE_OPEN ||
        request->extraAuth || (request->upperAuth & ~(APPLE80211_AUTHTYPE_WPA_PSK |
        APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK))) return kIOReturnUnsupported;
    AiritlWcl::SecurityPolicy security;
    const AiritlWcl::Error securityError = AiritlWcl::joinSecurityPolicy(*request, security);
    if (securityError != AiritlWcl::Error::none)
        return securityError == AiritlWcl::Error::unsupported ? kIOReturnUnsupported : kIOReturnBadArgument;
    // MFPC permits protection when both peers support it. It does not require
    // protection from a local adapter that lacks MFP. MFPR is rejected above.
    if ((ic->ic_caps & IEEE80211_C_MFP) &&
        ((request->capabilities & 2) ||
         (security.protocol == AiritlWcl::SecurityProtocol::rsn && !(security.capabilities & 0x80))))
        return kIOReturnUnsupported;
    uint32_t requestedProtocols = 0, requestedAkms = 0;
    if (request->upperAuth & APPLE80211_AUTHTYPE_WPA_PSK) requestedProtocols |= IEEE80211_PROTO_WPA;
    if (request->upperAuth & (APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK)) requestedProtocols |= IEEE80211_PROTO_RSN;
    if (request->upperAuth & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK)) requestedAkms |= IEEE80211_AKM_PSK;
    if (request->upperAuth & APPLE80211_AUTHTYPE_SHA256_PSK) requestedAkms |= IEEE80211_AKM_SHA256_PSK;
    uint32_t requestedCipher = 0, requestedGroup = 0;
    if (security.protocol != AiritlWcl::SecurityProtocol::none) {
        requestedProtocols = security.protocol == AiritlWcl::SecurityProtocol::rsn ? IEEE80211_PROTO_RSN : IEEE80211_PROTO_WPA;
        requestedAkms = ((security.akms & uint32_t(AiritlWcl::SecurityAkm::psk)) ? IEEE80211_AKM_PSK : 0) |
            ((security.akms & uint32_t(AiritlWcl::SecurityAkm::sha256Psk)) ? IEEE80211_AKM_SHA256_PSK : 0);
        requestedCipher = ((security.pairwiseCiphers & uint32_t(AiritlWcl::SecurityCipher::ccmp)) ? IEEE80211_CIPHER_CCMP : 0) |
            ((security.pairwiseCiphers & uint32_t(AiritlWcl::SecurityCipher::tkip)) ? IEEE80211_CIPHER_TKIP : 0);
        requestedGroup = security.group == AiritlWcl::SecurityCipher::ccmp ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP;
    }
    struct ieee80211_node *node = NULL;
    bool unsupportedCandidate = false;
    for (uint32_t i = 0; i < request->candidateCount; ++i) {
        uint32_t candidateAkms = requestedAkms;
        auto *candidate = ieee80211_find_node(ic, request->candidates[i].bssid);
        if (candidate && candidate->ni_state == IEEE80211_STA_COLLECT) continue;
        if (candidate) {
            // The cache retains one preferred security IE. A requested WPA1
            // offer can exist on a mixed AP without that retained WPA1 body.
            if (requestedAkms && !(candidate->ni_rsnprotos & requestedProtocols) &&
                (candidate->ni_supported_rsnprotos & requestedProtocols)) {
                unsupportedCandidate = true; continue;
            }
            if (!requestedAkms) {
                if (candidate->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) continue;
            } else if (!(candidate->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) ||
                       !(candidate->ni_rsnprotos & requestedProtocols) ||
                       !(candidate->ni_rsnakms & requestedAkms) ||
                       !(candidate->ni_rsnciphers & (IEEE80211_CIPHER_CCMP | IEEE80211_CIPHER_TKIP)) ||
                       (candidate->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP &&
                        candidate->ni_rsngroupcipher != IEEE80211_CIPHER_TKIP)) continue;
            if (requestedCipher && (!(candidate->ni_rsnciphers & requestedCipher) ||
                candidate->ni_rsngroupcipher != requestedGroup)) continue;
            const uint32_t allowedCiphers = requestedCipher ? requestedCipher :
                (IEEE80211_CIPHER_CCMP | IEEE80211_CIPHER_TKIP);
            if (requestedAkms && !(candidate->ni_rsnciphers & allowedCiphers & IEEE80211_CIPHER_CCMP)) {
                candidateAkms &= ~IEEE80211_AKM_SHA256_PSK;
                if (!candidateAkms) { unsupportedCandidate = true; continue; }
            }
            if (requestedAkms && (candidate->ni_rsncaps & IEEE80211_RSNCAP_MFPR) && !(ic->ic_caps & IEEE80211_C_MFP)) {
                unsupportedCandidate = true; continue;
            }
            if (security.protocol == AiritlWcl::SecurityProtocol::rsn &&
                ((candidate->ni_rsncaps & 0x3c) != (security.capabilities & 0x3c) || (ic->ic_flags & IEEE80211_F_PBAR))) {
                unsupportedCandidate = true; continue;
            }
        }
        if (candidate && candidate->ni_chan && candidate->ni_esslen == request->ssidLength &&
            !memcmp(candidate->ni_essid, request->ssid, request->ssidLength) &&
            !memcmp(candidate->ni_bssid, request->candidates[i].bssid, 6) &&
            ieee80211_chan2ieee(ic, candidate->ni_chan) == (request->candidates[i].channelSpec & 0xff)) {
            node = candidate; requestedAkms = candidateAkms; break;
        }
    }
    if (!node) return unsupportedCandidate ? kIOReturnUnsupported : kIOReturnNotFound;
    AiritlWclKey key = {};
    if (AiritlWcl::copyKey(request->key, sizeof(request->key), key) != AiritlWcl::Error::none) {
        bzero(&key, sizeof(key)); return kIOReturnBadArgument;
    }
    uint8_t pmk[32] = {};
    if (request->upperAuth) {
        if (key.cipher == 6 && key.length == sizeof(pmk)) {
            memcpy(pmk, key.data, sizeof(pmk));
        } else if (key.cipher == 10 && key.length >= 8 && key.length <= 63) {
            char password[65] = {};
            memcpy(password, key.data, key.length);
            if (strlen(password) != key.length ||
                pbkdf2_sha1(password, request->ssid, request->ssidLength, 4096, pmk, sizeof(pmk))) {
                bzero(password, sizeof(password)); bzero(&key, sizeof(key)); return kIOReturnBadArgument;
            }
            bzero(password, sizeof(password));
        } else if (key.cipher == 10 && key.length == 64) {
            for (unsigned i = 0; i < sizeof(pmk); ++i) {
                unsigned value = 0;
                for (unsigned j = 0; j < 2; ++j) {
                    uint8_t c = key.data[i * 2 + j];
                    unsigned digit;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                    else { bzero(pmk, sizeof(pmk)); bzero(&key, sizeof(key)); return kIOReturnBadArgument; }
                    value = value * 16 + digit;
                }
                pmk[i] = value;
            }
        } else { bzero(&key, sizeof(key)); return kIOReturnUnsupported; }
    } else if (key.length) { bzero(&key, sizeof(key)); return kIOReturnUnsupported; }
    struct ether_addr bssid;
    memcpy(bssid.octet, node->ni_bssid, 6);
    // node_join_bss copies the selected node into ic_bss but does not consume
    // a reference. Keep the cache node alive across configuration/HAL sleeps.
    ieee80211_ref_node(node);
    const uint64_t admissionGeneration = fNativeJoinState.generation;
    const uint64_t admissionCoreGeneration = ic->ic_native_core_generation;
    const uint64_t admissionAssociationGeneration = ic->ic_native_join_generation;
    auto *admissionBss = ic->ic_bss;
    fNativeJoinDispatching = true;
    IOReturn result = associateSSID(request->ssid, request->ssidLength, bssid,
                                    request->lowerAuth, request->upperAuth,
                                    pmk, request->upperAuth ? sizeof(pmk) : 0, 0);
    bzero(pmk, sizeof(pmk)); bzero(&key, sizeof(key));
    fNativeJoinDispatching = false;
    if (result != kIOReturnSuccess) { ieee80211_release_node(ic, node); return result; }
    if (!nativeControlReady() || fHalService->get80211Controller() != ic ||
        fNativeJoinState.generation != admissionGeneration || fNativeJoinState.active ||
        fNativeJoinState.draining || fNativeScanState.active || fNativeScanState.draining ||
        ic->ic_native_core_generation != admissionCoreGeneration || ic->ic_bss != admissionBss ||
        ic->ic_native_join_generation != admissionAssociationGeneration ||
        !instance->power_state || !(ic->ic_ac.ac_if.if_flags & IFF_RUNNING)) {
        ieee80211_release_node(ic, node); return kIOReturnNotReady;
    }
    if (fHalService->nativeKeyInstallPending() || !fHalService->nativeRegulatoryReady()) {
        ieee80211_release_node(ic, node); return kIOReturnBusy;
    }
    // A forced native cleanup can mark a held node while configuration sleeps.
    // Release it through the normal COLLECT path instead of joining stale data.
    if (node->ni_state == IEEE80211_STA_COLLECT) {
        ieee80211_release_node(ic, node); return kIOReturnNotReady;
    }
#ifdef USE_APPLE_SUPPLICANT
    bzero(ic->ic_rsn_ie_override, sizeof(ic->ic_rsn_ie_override));
    if (request->rsnLength) memcpy(ic->ic_rsn_ie_override, request->rsn, request->rsnLength);
#endif
    ic->ic_rsnakms = requestedAkms;
    ic->ic_rsnprotos = requestedProtocols;
    if (requestedCipher) {
        ic->ic_rsnciphers = requestedCipher;
        ic->ic_rsngroupcipher = static_cast<enum ieee80211_cipher>(requestedGroup);
    }
    if (ieee80211_match_bss(ic, node, 0)) {
        ieee80211_release_node(ic, node); return kIOReturnUnsupported;
    }
    current_authtype_lower = request->lowerAuth;
    current_authtype_upper = request->upperAuth;
    bzero(request->key, sizeof(request->key));
    bzero(request->secret, sizeof(request->secret));
    fNativeJoin = request;
    const uint64_t token = fNativeJoinState.begin();
    ic->ic_native_join_generation = token;
    ieee80211_clear_native_assoc_ie(ic);
    fNativeAssocReported = false; fNativeFirstBeaconReported = false; fNativeConnectReported = false;
    fNativeJoinAbortPending = false;
    memcpy(fNativeJoinBssid, bssid.octet, 6);
    // WCL chose this cached BSS. Do not start an unrelated automatic selection.
    ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
    clearNativeBss();
    if (!fNativeJoinState.active || fNativeJoinState.generation != token || !fNativeJoin ||
        !nativeControlReady() || fHalService->get80211Controller() != ic ||
        ic->ic_native_core_generation != admissionCoreGeneration || ic->ic_native_join_generation != token ||
        ic->ic_bss != admissionBss || fHalService->nativeKeyInstallPending()) {
        if (fNativeJoinState.active && fNativeJoinState.generation == token && fNativeJoin == request)
            handleHalJoinEvent(4, 1, token);
        ieee80211_release_node(ic, node); return kIOReturnSuccess;
    }
    ieee80211_node_join_bss(ic, node, 1);
    ieee80211_release_node(ic, node);
    return kIOReturnSuccess;
}
void AirportItlwmSkywalkInterface::handleHalJoinEvent(int event, int status, uint64_t token)
{
    if (!airitlInNativeGate()) {
        AiritlNativeCompletion call = {status, token, event};
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(2), &call);
        return;
    }
    if (token != fNativeJoinState.generation) return;
    if (fNativeRoam.active) { handleNativeRoamEvent(event, status, token); return; }
    if ((event == 3 || event == 5) && fNativeConnectReported) {
        uint8_t message[20] = {};
        AiritlWcl::write16(message + 8, uint16_t(status));
        memcpy(message + 10, fNativeJoinBssid, 6); message[16] = event == 3;
        fNativeConnectReported = false;
        clearNativeBss();
        if (fNativeJoinState.active && fNativeJoinState.generation == token) {
            fNativeJoinState.finish(token);
            releaseNativeJoin();
        }
        postMessage(0xcb, message, sizeof(message), true);
        return;
    }
    if (!fNativeJoinState.active || !fNativeJoin) return;
    if (event == 1 && !fNativeAssocReported) {
        uint8_t message[28];
        AiritlWcl::encodeAuthAssoc(message, fNativeJoinBssid, status ? uint16_t(status) : 0,
                                  status ? 0x3eb : 0, 0, 0);
        fNativeAssocReported = true;
        if (status) clearNativeBss();
        postMessage(0xd3, message, sizeof(message), true);
        if (fNativeJoinState.generation != token || !fNativeJoinState.active || !fNativeJoin) return;
        if (status) { fNativeJoinState.finish(token); releaseNativeJoin(); return; }
        pollNativeJoin();
    } else if (event == 2) {
        pollNativeJoin();
    } else if (event == 3 || event == 5) {
        uint8_t message[20] = {};
        AiritlWcl::write16(message + 8, uint16_t(status));
        memcpy(message + 10, fNativeJoinBssid, 6); message[16] = event == 3;
        clearNativeBss();
        postMessage(0xcb, message, sizeof(message), true);
        if (fNativeJoinState.generation != token || !fNativeJoinState.active || !fNativeJoin) return;
        uint8_t completion[164];
        AiritlWcl::encodeConnect(completion, fNativeJoinBssid, status ? uint16_t(status) : 12, 0xffff, 0, 0);
        postMessage(0xd5, completion, sizeof(completion), true);
        if (fNativeJoinState.generation != token || !fNativeJoinState.active || !fNativeJoin) return;
        fNativeJoinState.finish(token); releaseNativeJoin();
    } else if (event == 4) {
        clearNativeBss();
        if (fNativeAssocReported) {
            uint8_t message[164]; AiritlWcl::encodeConnect(message, fNativeJoinBssid, status ? uint16_t(status) : 1, 0xffff, 0, 0);
            postMessage(0xd5, message, sizeof(message), true);
        } else {
            uint8_t message[28]; AiritlWcl::encodeAuthAssoc(message, fNativeJoinBssid, status ? uint16_t(status) : 1, 0x3eb, 0, 0);
            postMessage(0xd3, message, sizeof(message), true);
        }
        if (fNativeJoinState.generation == token && fNativeJoinState.active && fNativeJoin) {
            fNativeJoinState.finish(token); releaseNativeJoin();
        }
    }
}
void AirportItlwmSkywalkInterface::pollNativeJoin()
{
    if (!airitlInNativeGate()) {
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(4));
        return;
    }
    if (fNativeRoam.active) { pollNativeRoam(); return; }
    if (fNativeJoinDispatching || !fNativeJoinState.active || !fNativeJoin || !fNativeAssocReported || !fNativeFirstBeaconReported ||
        fNativeConnectReported || !fHalService) return;
    const uint64_t token = fNativeJoinState.generation;
    uint8_t bssid[6]; memcpy(bssid, fNativeJoinBssid, sizeof(bssid));
    if (!nativeJoinCompletionMatches(token, bssid)) return;
    fNativeJoinDispatching = true;
    if (!fNativeLinkReported) {
        uint8_t link[12];
        AiritlWcl::encodeLinkStatus(link, bssid, true, IO80211InfraInterface::isDebounceOnGoing());
        fNativeLinkReported = true;
        postMessage(0xd8, link, sizeof(link), true);
    }
    if (!nativeJoinCompletionMatches(token, bssid) || !fNativeLinkReported || !fNativeBssConfigured) {
        fNativeJoinDispatching = false;
        return;
    }
    uint8_t message[164]; AiritlWcl::encodeConnect(message, bssid, 0, 0, 0, 0);
    fNativeConnectReported = true;
    postMessage(0xd5, message, sizeof(message), true);
    fNativeJoinDispatching = false;
    if (!nativeJoinCompletionMatches(token, bssid) || !fNativeConnectReported) return;
    fNativeJoinState.finish(token);
    releaseNativeJoin();
}
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(void *)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeAbortGated, this, reinterpret_cast<void *>(1));
}

void AirportItlwmSkywalkInterface::cancelNativeRequests(IOReturn reason)
{
    if (!airitlInNativeGate()) {
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(5), &reason);
        return;
    }
    if (!++fNativeControlSequence) ++fNativeControlSequence;
    if (fNativeRoam.draining) fNativeJoinState.cancel();
    cancelNativeRoam();
    fNativeConnectReported = false;
    clearNativeBss();
    if (fNativeScanState.active) {
        const bool submitted = fNativeScanState.submitted;
        const uint64_t token = fNativeScanState.cancel();
        if (submitted && fHalService) fHalService->cancelNativeScan(token);
        releaseNativeScan();
        uint32_t status = AiritlWcl::scanStatus(false, true, false);
        postMessage(0xed, &status, sizeof(status), true);
    }
    if (fNativeJoinState.active) {
        fNativeJoinState.cancel(); fNativeJoinAbortPending = true; releaseNativeJoin();
    }
}
void AirportItlwmSkywalkInterface::nativeResetBegin()
{
    if (!airitlInNativeGate()) return;
    fNativeResetPending = fNativeResetQuiescencePending = true;
    fNativeObserverReady = false;
    // The current HAL stop owns destruction. Never call public abort here.
    cancelNativeRequests(kIOReturnAborted);
    // No WCL driver reset is posted for internal recovery, so WCL does not end
    // its current join itself. Keep the pending D6 abort; nativeHalQuiesced
    // reports it once the HAL has drained.
    fNativeAssocReported = fNativeFirstBeaconReported = false;
    fNativeLinkReported = fNativeJoinDispatching = false;
    bzero(fNativeJoinBssid, sizeof(fNativeJoinBssid));
}

void AirportItlwmSkywalkInterface::nativeHalQuiesced()
{
    if (!airitlInNativeGate()) {
        if (_fCommandGate) _fCommandGate->runAction(nativeEventGated, this, reinterpret_cast<void *>(6));
        return;
    }
    fNativeScanState.quiesced(); fNativeJoinState.quiesced();
    if (fNativeResetQuiescencePending) {
        bzero(&fNativeRoam, sizeof(fNativeRoam));
        fNativeResetQuiescencePending = false;
    }
    if (fNativeRoam.draining) {
        fNativeRoam.hardwareQuiesced = true;
        if (fNativeRoam.downAcknowledged) fNativeRoam.draining = false;
    }
    clearNativeBss();
    releaseNativeScan(); releaseNativeJoin();
    if (fNativeJoinAbortPending) { fNativeJoinAbortPending = false; postMessage(0xd6, NULL, 0, true); }
}
IOReturn AirportItlwmSkywalkInterface::setNativeCipherKey(const void *input)
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    AiritlWclKey key;
    if (AiritlWcl::copyKey(input, sizeof(key), key) != AiritlWcl::Error::none) {
        bzero(&key, sizeof(key)); return kIOReturnBadArgument;
    }
    IOReturn result = _fCommandGate->runAction(nativeCipherGated, this, &key);
    bzero(&key, sizeof(key));
    return result;
}
IOReturn AirportItlwmSkywalkInterface::nativeCipherGated(OSObject *owner, void *object, void *input, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    auto *key = static_cast<AiritlWclKey *>(input);
    if (!self || self->instance != owner || !key || !self->fHalService) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    auto *ic = self->fHalService->get80211Controller();
    if (!ic || !ic->ic_bss) return kIOReturnNotAttached;
    if (!(ic->ic_ac.ac_if.if_flags & IFF_RUNNING)) return kIOReturnNotReady;
    if (key->cipher == APPLE80211_CIPHER_PMK) {
        if (key->length != sizeof(ic->ic_psk) || !self->fNativeJoinState.active) return kIOReturnBadArgument;
        memcpy(ic->ic_psk, key->data, sizeof(ic->ic_psk)); ic->ic_flags |= IEEE80211_F_PSK;
        return kIOReturnSuccess;
    }
    if (key->cipher != APPLE80211_CIPHER_AES_CCM && key->cipher != APPLE80211_CIPHER_TKIP)
        return kIOReturnUnsupported;
    const bool unicast = (key->flags & APPLE80211_KEY_FLAG_UNICAST) != 0;
    const bool multicast = (key->flags & APPLE80211_KEY_FLAG_MULTICAST) != 0;
    if (unicast && multicast) return kIOReturnBadArgument;
    uint8_t any = 0; for (unsigned i = 0; i < 6; ++i) any |= key->bssid[i];
    const bool addressedToBss = any && !memcmp(key->bssid, ic->ic_bss->ni_bssid, 6);
    const bool groupAddress = (key->bssid[0] & 1) != 0;
    const bool pairwise = unicast || (!multicast && !groupAddress && addressedToBss && key->index == 0);
    if (!pairwise && !multicast && !groupAddress) return kIOReturnBadArgument;
    if (pairwise && (key->index || (!addressedToBss && any))) return kIOReturnBadArgument;
    const uint32_t cipher = key->cipher == APPLE80211_CIPHER_AES_CCM ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP;
    if ((pairwise ? ic->ic_bss->ni_rsncipher : ic->ic_bss->ni_rsngroupcipher) != cipher)
        return kIOReturnBadArgument;
    if (key->length != (cipher == IEEE80211_CIPHER_TKIP ? 32U : 16U)) return kIOReturnBadArgument;
    if (!pairwise && (key->rscLength < 6 || key->index >= 4 ||
        !ic->ic_bss->ni_pairwise_key.k_len || !(ic->ic_bss->ni_flags & IEEE80211_NODE_RXPROT)))
        return kIOReturnBadArgument;
    uint8_t material[32] = {};
    memcpy(material, key->data, key->length);
#ifndef USE_APPLE_SUPPLICANT
    if (cipher == IEEE80211_CIPHER_TKIP) {
        // Apple supplies TX MIC then RX MIC. The internal STA cipher uses RX then TX.
        memcpy(material + 16, key->data + 24, 8);
        memcpy(material + 24, key->data + 16, 8);
    }
#endif
    IOReturn result = pairwise ? self->setPTK(material, key->length) :
        self->setGTK(material, key->length, key->index, key->rsc);
    bzero(material, sizeof(material));
    if (result == kIOReturnSuccess) self->pollNativeJoin();
    else self->handleHalJoinEvent(4, 1, self->nativeJoinGeneration());
    return result;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *input)
{
#ifdef AIRITL_ENABLE_BSDIF
    if (!nativeControlReady() || !input) return kIOReturnNotReady;
#else
    if (!input) return kIOReturnNotReady;
#endif
    apple80211_wcl_update_link_state request;
    memcpy(&request, input, sizeof(request));
    if (request.linkUp > 1 || request.publishLinkState > 1 || request.updateBss > 1) return kIOReturnBadArgument;
#ifndef AIRITL_ENABLE_BSDIF
    if (request.linkUp ? !nativeControlReady() : !_fCommandGate) return kIOReturnNotReady;
#endif
    return _fCommandGate->runAction(nativeLinkGated, this, &request);
}
IOReturn AirportItlwmSkywalkInterface::nativeLinkGated(OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    auto *request = static_cast<apple80211_wcl_update_link_state *>(argument);
#ifdef AIRITL_ENABLE_BSDIF
    if (!self || self->instance != owner || !self->fHalService || !request) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    auto *ic = self->fHalService->get80211Controller();
    if (!ic) return kIOReturnNotAttached;
#else
    if (!self || !request) return kIOReturnNotAttached;
    if (request->linkUp > 1 || request->publishLinkState > 1 || request->updateBss > 1)
        return kIOReturnBadArgument;
#endif
    IOCommandGate *gate = _fCommandGate;
    IOWorkLoop *loop = gate ? gate->getWorkLoop() : nullptr;
    if (!gate || !loop || loop != _fWorkloop || !loop->inGate() || loop->onThread()) {
        XYLog("%s invalid native link-call context\n", __FUNCTION__);
        return kIOReturnNotPermitted;
    }
#ifndef AIRITL_ENABLE_BSDIF
    if (self->instance != owner) return kIOReturnNotAttached;
    if (request->linkUp ? !self->nativeControlReady() : !self->nativeLinkDownReady())
        return kIOReturnNotReady;
    auto *ic = self->fHalService->get80211Controller();
    if (!ic) return kIOReturnNotAttached;
#endif
    if (request->linkUp) {
        if (ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss ||
            memcmp(request->bssid, ic->ic_bss->ni_bssid, 6) ||
            ((ic->ic_flags & IEEE80211_F_RSNON) && !ic->ic_bss->ni_port_valid)) return kIOReturnNotReady;
        struct ether_addr address;
        memcpy(address.octet, request->bssid, sizeof(address.octet));
        apple80211_channel_data channel = {};
        if (request->updateBss) {
            if (self->getCHANNEL(&channel) != kIOReturnSuccess) return kIOReturnNotReady;
            self->setCurrentApAddress(&address);
        }
#ifdef AIRITL_LINK_DEFER
        if (request->publishLinkState) {
            AIRITL_DBG("%s native link up publish (updateBss=%u)\n", __FUNCTION__, request->updateBss);
            self->instance->noteNativeLinkPublication(true);
        }
#endif
        if (request->publishLinkState && !self->setLinkState(kIO80211NetworkLinkUp, 0, false, 0))
            return kIOReturnError;
        if (request->updateBss) {
#ifdef AIRITL_ENABLE_BSDIF
            if (self->linkState() != kIO80211NetworkLinkUp) return kIOReturnNotReady;
#else
            if (request->publishLinkState && self->linkState() != kIO80211NetworkLinkUp) return kIOReturnNotReady;
#endif
            // Refresh properties from the actual HAL BSS and channel.
            // Nonpublishing setup does not require native UP or a peer.
            self->updateBSSIDProperty(address, channel.channel, true);
            self->updateSSIDProperty();
        }
        return kIOReturnSuccess;
    }
#ifdef AIRITL_LINK_DEFER
    AIRITL_DBG("%s native link down publish (debounce=%u)\n", __FUNCTION__, request->debounceMilliseconds);
    self->instance->noteNativeLinkPublication(false);
#endif
#ifndef AIRITL_ENABLE_BSDIF
    self->setCurrentApAddress(nullptr);
#endif
    if (!self->setLinkState(kIO80211NetworkLinkDown, 0, request->debounceMilliseconds != 0,
                            request->debounceMilliseconds)) return kIOReturnError;
    self->acknowledgeNativeRoamDown(request->bssid);
    return kIOReturnSuccess;
}
IOReturn AirportItlwmSkywalkInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *input)
{
    if (!nativeControlReady() || !input) return kIOReturnNotReady;
    apple80211_leave_network request;
    memcpy(&request, input, sizeof(request));
    // methodControl selects the frame (Apple leaveNetworkSync): non-zero ->
    // deauthenticate for reason, zero -> disassociate. It is no longer refused.
    if (request.involuntary > 1 || request.abortJoin > 1) return kIOReturnUnsupported;
    return _fCommandGate->runAction(nativeLeaveGated, this, &request);
}
IOReturn AirportItlwmSkywalkInterface::nativeLeaveGated(OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    auto *request = static_cast<apple80211_leave_network *>(argument);
    if (!self || self->instance != owner || !self->fHalService || !request) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    auto *ic = self->fHalService->get80211Controller();
    if (!ic) return kIOReturnNotAttached;
    uint8_t any = 0; for (unsigned i = 0; i < 6; ++i) any |= request->bssid[i];
    if (any && (!ic->ic_bss || memcmp(request->bssid, ic->ic_bss->ni_bssid, 6))) return kIOReturnBadArgument;
    if (ic->ic_state >= IEEE80211_S_ASSOC && ic->ic_bss) {
        const bool deauth = request->methodControl != 0;
        const int reason = request->ieeeReason ? request->ieeeReason :
            (deauth ? IEEE80211_REASON_AUTH_LEAVE : IEEE80211_REASON_ASSOC_LEAVE);
        AIRITL_DBG("native leave: %s reason %d (methodControl %u)\n",
              deauth ? "deauth" : "disassoc", reason, (unsigned)request->methodControl);
        IEEE80211_SEND_MGMT(ic, ic->ic_bss,
                           deauth ? IEEE80211_FC0_SUBTYPE_DEAUTH : IEEE80211_FC0_SUBTYPE_DISASSOC,
                           reason);
    }
    self->disassocIsVoluntary = !request->involuntary;
    self->cancelNativeRequests(kIOReturnAborted);
    return self->instance->abortNativeJoin();
}

#endif
