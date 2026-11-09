#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::getHW_ADDR(apple80211_hw_mac_address *output)
{
    if (!output) return kIOReturnBadArgument;
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeHardwareAddressGated, this, output);
}

IOReturn AirportItlwmSkywalkInterface::nativeHardwareAddressGated(
    OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    auto *output = static_cast<apple80211_hw_mac_address *>(argument);
    if (!output) return kIOReturnNotReady;
    apple80211_hw_mac_address result = {};
    if (!self->fHalService->copyPermanentHardwareAddress(result.address)) return kIOReturnNotReady;
    uint8_t present = 0;
    for (unsigned i = 0; i < 6; ++i) present |= result.address[i];
    if (!present || (result.address[0] & 1)) return kIOReturnNotReady;
    result.version = 1;
    memcpy(output, &result, sizeof(result));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *output)
{
    if (!output) return kIOReturnBadArgument;
    if (!nativeControlReady()) return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeChannelInfoGated, this, output);
}

IOReturn AirportItlwmSkywalkInterface::nativeChannelInfoGated(
    OSObject *owner, void *object, void *argument, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(object));
    if (!self || self->instance != owner) return kIOReturnNotAttached;
    if (!self->nativeControlReady()) return kIOReturnNotReady;
    if (!self->fHalService->nativeRegulatoryReady()) return kIOReturnNotReady;
    auto *output = static_cast<apple80211ChannelInfo *>(argument);
    auto *ic = self->fHalService->get80211Controller();
    auto *info = self->fHalService->getDriverInfo();
    if (!output || !ic || !info) return kIOReturnNotReady;
    const char *country = info->getFirmwareCountryCode();
    if (!country || !country[0] || !country[1]) return kIOReturnNotReady;

    // The current native scan adapter supports 2.4/5 GHz. Publish one real
    // 20 MHz primary-channel entry per HAL-available channel. No 6 GHz power,
    // wide-channel combination or regulatory permission is synthesized.
    auto *result = static_cast<apple80211ChannelInfo *>(IOMallocZero(sizeof(*output)));
    if (!result) return kIOReturnNoMemory;
    for (unsigned channel = 1; channel <= IEEE80211_CHAN_MAX; ++channel) {
        const auto &hal = ic->ic_channels[channel];
        if (!hal.ic_freq || !isset(ic->ic_chan_avail, channel)) continue;
        const bool twoGHz = (hal.ic_flags & IEEE80211_CHAN_2GHZ) != 0;
        const bool fiveGHz = (hal.ic_flags & IEEE80211_CHAN_5GHZ) != 0;
        if (twoGHz == fiveGHz || (twoGHz && channel > 14) || (fiveGHz && channel >= 197)) continue;
        if (result->count == sizeof(result->channels) / sizeof(result->channels[0])) {
            IOFree(result, sizeof(*result));
            return kIOReturnNoSpace;
        }
        auto &entry = result->channels[result->count++];
        entry.channelSpec = static_cast<uint16_t>(channel | (fiveGHz ? 0xd000 : 0x1000));
        entry.primaryChannel = static_cast<uint8_t>(channel);
        // Native BCM producer and consumers map DFS to bit 1, passive to bit 2.
        if (hal.ic_flags & IEEE80211_CHAN_DFS) entry.flags |= 2;
        if (hal.ic_flags & IEEE80211_CHAN_PASSIVE) entry.flags |= 4;
    }
    if (!result->count) {
        IOFree(result, sizeof(*result));
        return kIOReturnNotReady;
    }
    memcpy(result->country, country, sizeof(result->country));
    memcpy(output, result, sizeof(*result));
    IOFree(result, sizeof(*result));
    return kIOReturnSuccess;
}
#endif
