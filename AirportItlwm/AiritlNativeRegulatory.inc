#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::publishNativeRegulatoryUpdate()
{
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeRegulatoryGated, this);
}

IOReturn AirportItlwmSkywalkInterface::nativeRegulatoryGated(
    OSObject *owner, void *object, void *, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    if (!self->nativeControlReady() || !self->fHalService->nativeRegulatoryReady())
        return kIOReturnNotReady;
    // The firmware response has already committed channels, bitmaps and its
    // actual country. Native event 0xe0 reloads both Infra and WCL channel data.
    // Event 11 refreshes the country property. Both native carriers are empty.
    self->postMessage(0xe0, nullptr, 0, true);
    self->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, true);
    return kIOReturnSuccess;
}
#endif
