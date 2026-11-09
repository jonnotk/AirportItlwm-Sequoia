#ifndef IOSkywalkEthernetInterface_h
#define IOSkywalkEthernetInterface_h

#include "IOSkywalkNetworkInterface.h"

struct nicproxy_limits_info_s;
struct nicproxy_info_s;

class IOSkywalkEthernetInterface : public IOSkywalkNetworkInterface {
    OSDeclareAbstractStructors( IOSkywalkEthernetInterface )
    
public:
    struct RegistrationInfo {
        uint8_t pad[304];

        // The pinned host's copyRegistrationInfo reads six MAC bytes here.
        enum { kHardwareAddressOffset = 0x108 };
        void setHardwareAddress(const ether_addr &address)
        {
            static_assert(kHardwareAddressOffset + sizeof(address.octet) <= sizeof(pad), "MAC outside registration record");
            memcpy(pad + kHardwareAddressOffset, address.octet, sizeof(address.octet));
        }
#if __IO80211_TARGET >= __MAC_15_0
        void setWiFiDiscoveryIdentity()
        {
            // Pinned native consumers: subfamily feeds ifnet_init_eparams;
            // the media words seed ifm_active and ifm_current respectively.
            const uint32_t wifiSubfamily = 3;
            const uint32_t ieee80211Media = 0x80;
            static_assert(0x28 + sizeof(ieee80211Media) <= sizeof(pad), "Media outside registration record");
            memcpy(pad + 0x0c, &wifiSubfamily, sizeof(wifiSubfamily));
            memcpy(pad + 0x24, &ieee80211Media, sizeof(ieee80211Media));
            memcpy(pad + 0x28, &ieee80211Media, sizeof(ieee80211Media));
        }
#endif
    } __attribute__((packed));
    static_assert(sizeof(RegistrationInfo) == 0x130, "Invalid registration record size");
    
public:
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient( task_t owningTask, void * securityID,
                                   UInt32 type, OSDictionary * properties,
                                   LIBKERN_RETURNS_RETAINED IOUserClient ** handler ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(
                                   unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enable(UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t,UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t,UInt,void *) APPLE_KEXT_OVERRIDE;
    virtual void *getPacketTapInfo(UInt *,UInt *) APPLE_KEXT_OVERRIDE;
    virtual void enableNetworkWake(UInt) APPLE_KEXT_OVERRIDE;
    virtual UInt getMaxTransferUnit(void) APPLE_KEXT_OVERRIDE;
    virtual UInt getMinPacketSize(void) APPLE_KEXT_OVERRIDE;
    virtual void *getInterfaceFamily(void) APPLE_KEXT_OVERRIDE;
    virtual void *getInterfaceSubFamily(void) APPLE_KEXT_OVERRIDE;
    virtual UInt getInitialMedia(void) APPLE_KEXT_OVERRIDE;
    virtual const char *getBSDNamePrefix(void) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_15_0
    virtual IOReturn registerNetworkInterfaceWithLogicalLink(IOSkywalkEthernetInterface::RegistrationInfo const*, IOSkywalkLogicalLink*, IOSkywalkPacketBufferPool*, IOSkywalkPacketBufferPool*, UInt);
#endif
    virtual void getHardwareAddress(ether_addr *);
    virtual void setHardwareAddress(ether_addr *);
    virtual void setLinkLayerAddress(ether_addr *);
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn configureMulticastFilter(UInt,ether_addr const*,UInt);
    virtual IOReturn setMulticastAddresses(ether_addr const*,UInt);
    virtual IOReturn setAllMulticastModeEnable(bool);
#else
    virtual bool configureMulticastFilter(UInt,ether_addr const*,UInt);
    virtual bool setMulticastAddresses(ether_addr const*,UInt);
    virtual void setAllMulticastModeEnable(bool);
#endif
    virtual IOReturn setPromiscuousModeEnable(bool, UInt);
    virtual void reportNicProxyLimits(nicproxy_limits_info_s);
    virtual void hwConfigNicProxyData(nicproxy_info_s *);
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  0 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  1 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  2 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  3 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  4 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  5 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  6 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  7 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  8 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface,  9 );
    OSMetaClassDeclareReservedUnused( IOSkywalkEthernetInterface, 10 );
    
public:
    bool initRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo*, unsigned int, unsigned long);
    IOReturn registerEthernetInterface(IOSkywalkEthernetInterface::RegistrationInfo const*, IOSkywalkPacketQueue**, unsigned int, IOSkywalkPacketBufferPool*, IOSkywalkPacketBufferPool*, unsigned int);
    IOReturn deregisterEthernetInterface(unsigned int options = 0);
    
public:
    void *vptr;
    uint8_t pad1[0x30];
    struct ExpansionData
    {
        RegistrationInfo *fRegistrationInfo;
        ifnet_t fBSDInterface;
    };
    ExpansionData *mExpansionData2;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(__offsetof(IOSkywalkEthernetInterface, mExpansionData2) == 0x108, "Invalid class size");
#pragma clang diagnostic pop

static_assert(sizeof(IOSkywalkEthernetInterface) == 0x110, "Invalid class size");

#endif /* IOSkywalkEthernetInterface_h */
