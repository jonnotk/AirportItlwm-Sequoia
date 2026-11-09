#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::setMacAddress(ether_addr &addr)
{
    if (!instance || !_fCommandGate)
        return kIOReturnNotAttached;
    return _fCommandGate->runAction(nativeSetMacGated, this, &addr);
}

IOReturn AirportItlwmSkywalkInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    // This interface has no low-latency/P2P coordinator. The native BCM getter
    // writes this exact four-byte zero state when its coordinator is absent.
    data->trafficOngoing = 0;
    data->p2pRealtimeOn = 0;
    data->desiredChannel = 0;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::waitForNativeReady()
{
    if (!airitlInNativeGate() || !instance || instance->fStopping || !fHalService)
        return kIOReturnNotAttached;
    if (fNativeControlOwner || fNativeJoinDispatching || fNativeRoam.active ||
        fNativeRoam.draining || fNativeScanState.active || fNativeScanState.draining ||
        fNativeJoinState.active || fNativeJoinState.draining)
        return kIOReturnBusy;
    auto *controller = instance;
    auto *hal = fHalService;
    auto *ic = hal->get80211Controller();
    if (!ic) return kIOReturnNotReady;
    if (!++fNativeControlSequence) ++fNativeControlSequence;
    const uint64_t token = fNativeControlSequence;
    fNativeControlOwner = token;
    const uint64_t scanGeneration = fNativeScanState.generation;
    const uint64_t joinGeneration = fNativeJoinState.generation;
    const auto power = controller->power_state;
    const bool observerReady = fNativeObserverReady;
    auto *gate = _fCommandGate;
    retain(); controller->retain(); hal->retain();
    // Five seconds total, below ordinary native join's default 17 seconds.
    // Wakeups do not restart this control deadline.
    IOReturn result = hal->nativeWaitUntilReady(5000000000ULL,
        &fNativeControlSequence, token);
    if (!airitlInNativeGate() || _fCommandGate != gate ||
        fNativeObserverReady != observerReady || instance != controller ||
        fHalService != hal || controller->fStopping ||
        hal->get80211Controller() != ic || fNativeControlOwner != token ||
        fNativeControlSequence != token || controller->power_state != power ||
        fNativeScanState.generation != scanGeneration ||
        fNativeJoinState.generation != joinGeneration || fNativeJoinDispatching ||
        fNativeRoam.active || fNativeRoam.draining || fNativeScanState.active ||
        fNativeScanState.draining || fNativeJoinState.active || fNativeJoinState.draining)
        result = kIOReturnAborted;
    if (result == kIOReturnSuccess && !hal->nativeRegulatoryReady())
        result = kIOReturnNotReady;
    if (fNativeControlOwner == token) fNativeControlOwner = 0;
    hal->release(); controller->release(); release();
    return result;
}

IOReturn AirportItlwmSkywalkInterface::nativeSetMacGated(OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    const auto *input = static_cast<const ether_addr *>(argument);
    if (!self || self->instance != owner || !input) return kIOReturnBadArgument;
    ether_addr address = *input;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < sizeof(address.octet); ++i) nonzero |= address.octet[i];
    if (!nonzero || (address.octet[0] & 1)) return kIOReturnBadArgument;
    IOReturn result = self->waitForNativeReady();
    if (result != kIOReturnSuccess) return result;
    // No gate release follows revalidation. Full MCC readiness excludes the
    // HAL setter's MCC sleep branch here.
    return self->fHalService->nativeSetHardwareAddress(address.octet);
}
#endif
