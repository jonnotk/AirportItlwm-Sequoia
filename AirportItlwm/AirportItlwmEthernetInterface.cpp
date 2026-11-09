//
//  AirportItlwmEthernetInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmEthernetInterface.hpp"

#include <sys/_if_ether.h>
#include <net80211/ieee80211_var.h>

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportItlwmEthernetInterface, IOEthernetInterface);

bool AirportItlwmEthernetInterface::
initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface)
{
    bool ret = super::init(controller);
    if (ret)
        this->interface = interface;
    this->isAttach = false;
    return ret;
}

IOReturn AirportItlwmEthernetInterface::
attachToDataLinkLayer( IOOptionBits options, void *parameter )
{
    XYLog("%s\n", __FUNCTION__);
    char infName[IFNAMSIZ];
    IOReturn ret = super::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && interface) {
        UInt8 builtIn = 0;
        IOEthernetAddress addr;
        interface->setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
        snprintf(infName, sizeof(infName), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        interface->setProperty("IOInterfaceName", OSString::withCString(infName));
        interface->setProperty(kIOInterfaceUnit, OSNumber::withNumber(ifnet_unit(getIfnet()), 8));
        interface->setProperty(kIOInterfaceNamePrefix, OSString::withCString(ifnet_name(getIfnet())));
        // BUG: getController() is an IO80211Controller,
        // NOT an IOEthernetController, so OSDynamicCast(IOEthernetController, ...) returns NULL and the
        // immediate ->getHardwareAddress() dereferences NULL -> wild dispatch -> triple-fault during the
        // BSD/IONetworkStack attach at the verbose->GUI handoff. AirportItlwm DOES override getHardwareAddress
        // (an IOEthernetController-signature method it inherits via IO80211Controller's chain), so call it
        // directly with a null guard instead of the bogus cast.
        // getHardwareAddress(IOEthernetAddress*) is a virtual on IO80211Controller (AirportItlwm's base),
        // so reach it via an IO80211Controller* cast (no circular AirportItlwm include needed).
        IO80211Controller *ctrl = OSDynamicCast(IO80211Controller, getController());
        if (ctrl && ctrl->getHardwareAddress(&addr) == kIOReturnSuccess)
            setProperty(kIOMACAddress,  (void *) &addr,
                        kIOEthernetAddressSize);
        // REMOVED (de-bridge): interface->registerService(); — fNetIf is already registered at CP9
        // (attachInterface). Re-registering from the ethernet attach is a redundant Skywalk-bridge call.
        // REMOVED (RE-confirmed CP10 crash): interface->prepareBSDInterface(getIfnet(), 0);
        // `interface` is the Skywalk fNetIf; this called Apple's IOSkywalkNetworkInterface::prepareBSDInterface
        // which dereferences mExpansionData->fRegistrationInfo+0x4c (the MTU). But fRegistrationInfo is only
        // populated at CP11-13, AFTER this attach (CP10) runs -> NULL+0x4c wild dispatch -> triple-fault. It is
        // also the WRONG call: prepareBSDInterface belongs to the Skywalk interface's OWN (deferred) BSD attach,
        // not the legacy IOEthernetInterface. This bsdInterface carries data via the normal IOEthernet mbuf path
        // (inputPacket/enqueueOutputPacket) and does not need to push its ifnet into the Skywalk interface.
//        ret = bpf_attach(getIfnet(), DLT_RAW, 0x48, &AirportItlwmEthernetInterface::bpfOutputPacket, &AirportItlwmEthernetInterface::bpfTap);
    }
    isAttach = true;
    return ret;
}

void AirportItlwmEthernetInterface::
detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
    super::detachFromDataLinkLayer(options, parameter);
    isAttach = false;
}

/**
 De-bridged: was `return isAttach ? this->interface : super::getProvider();` — a hack that, AFTER attach,
 reported the Skywalk interface as this IOEthernetInterface's provider "to avoid a skywalkfamily instance
 cast panic". But it only fired post-attach — exactly when IONetworkStack/configd enumerate the interface
 at the GUI handoff — feeding them a non-IONetworkController object => wrong-vtable dispatch => triple-fault.
 The hack only existed to support the Skywalk bridging we have now removed (prepareBSDInterface/registerService),
 so the bsdInterface is now a plain IOEthernetInterface whose provider is its real controller. Return that.
 */
IOService *AirportItlwmEthernetInterface::
getProvider() const
{
    return super::getProvider();
}

errno_t AirportItlwmEthernetInterface::
bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type, mbuf_t packet)
{
    XYLog("%s data_link_type: %d\n", __FUNCTION__, data_link_type);
    AirportItlwmEthernetInterface *networkInterface = (AirportItlwmEthernetInterface *)ifnet_softc(interface);
    return networkInterface->enqueueOutputPacket(packet);
}

errno_t AirportItlwmEthernetInterface::
bpfTap(ifnet_t interface, u_int32_t data_link_type, bpf_tap_mode direction)
{
    XYLog("%s data_link_type: %d direction: %d\n", __FUNCTION__, data_link_type, direction);
    return 0;
}

bool AirportItlwmEthernetInterface::
setLinkState(IO80211LinkState state)
{
    if (state == kIO80211NetworkLinkUp) {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) | (IFF_UP | IFF_RUNNING), (IFF_UP | IFF_RUNNING));
    } else {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) & ~(IFF_UP | IFF_RUNNING), 0);
    }
    return true;
}

extern const char* hexdump(uint8_t *buf, size_t len);

UInt32 AirportItlwmEthernetInterface::
inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
    ether_header_t *eh;
    size_t len = mbuf_len(packet);
    
    eh = (ether_header_t *)mbuf_data(packet);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(packet), len);
        XYLog("input EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    return IOEthernetInterface::inputPacket(packet, length, options, param);
}
