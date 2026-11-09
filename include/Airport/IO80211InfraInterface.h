//
//  IO80211InfraInterface.h
//  itlwm
//
//  Created by qcwap on 2023/6/12.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IO80211InfraInterface_h
#define IO80211InfraInterface_h

struct apple80211_wcl_advisory_info;
struct apple80211_wcl_tx_rx_latency;

class IO80211InfraInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211InfraInterface)
    
public:
    virtual bool init() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t, UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t, UInt, void *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;
    virtual UInt getHardwareAssists(void) APPLE_KEXT_OVERRIDE;
    virtual bool bpfTap(UInt,UInt) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET < __MAC_15_0
    virtual void getHardwareAddress(ether_addr *) APPLE_KEXT_OVERRIDE;
    virtual void setHardwareAddress(ether_addr *) APPLE_KEXT_OVERRIDE;
#endif
    virtual void postMessage(UInt,void *,unsigned long,bool) APPLE_KEXT_OVERRIDE;
    virtual IOReturn recordOutputPackets(TxSubmissionDequeueStats *,TxSubmissionDequeueStats *) APPLE_KEXT_OVERRIDE;
    virtual void logTxPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,apple80211_wme_ac,bool) APPLE_KEXT_OVERRIDE;
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool
#if __IO80211_TARGET >= __MAC_15_0
                                      ,bool
#endif
                                      ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn recordCompletionPackets(TxCompletionEnqueueStats *,TxCompletionEnqueueStats *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *
#if __IO80211_TARGET >= __MAC_15_0
                                 ,bool
#endif
                                 ) APPLE_KEXT_OVERRIDE;
    virtual SInt64 pendingPackets(unsigned char) APPLE_KEXT_OVERRIDE;
    virtual SInt64 packetSpace(unsigned char) APPLE_KEXT_OVERRIDE;
    virtual bool isDebounceOnGoing(void) APPLE_KEXT_OVERRIDE;
    virtual bool setLinkState(IO80211LinkState,UInt,bool debounceTimeout = 30,UInt code = 0) APPLE_KEXT_OVERRIDE;
    virtual IO80211LinkState linkState(void) APPLE_KEXT_OVERRIDE;
    virtual void setScanningState(UInt,bool,apple80211_scan_data *,int) APPLE_KEXT_OVERRIDE;
    virtual void setDataPathState(bool) APPLE_KEXT_OVERRIDE;
    virtual void *getScanManager(void) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkParameters(apple80211_interface_availability *) APPLE_KEXT_OVERRIDE;
    virtual void updateInterfaceCoexRiskPct(unsigned long long) APPLE_KEXT_OVERRIDE;
    virtual void setLQM(unsigned long long) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkStatus(void) APPLE_KEXT_OVERRIDE;
    virtual void updateLinkStatusGated(void) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceExtendedCCA(apple80211_channel,apple80211_cca_report *) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceCCA(apple80211_channel,int) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceNF(apple80211_channel,long long) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceOFDMDesense(apple80211_channel,long long) APPLE_KEXT_OVERRIDE;
    virtual void setDebugFlags(unsigned long long,UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt64 debugFlags(void) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceChipCounters(apple80211_stat_report *,apple80211_chip_counters_tx *,apple80211_chip_error_counters_tx *,apple80211_chip_counters_rx *) APPLE_KEXT_OVERRIDE;
    virtual void setInterfaceMIBdot11(apple80211_stat_report *,apple80211_ManagementInformationBasedot11_counters *) APPLE_KEXT_OVERRIDE;
    virtual void setFrameStats(apple80211_stat_report *,apple80211_frame_counters *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_14_4
    virtual void setInfraSpecificFrameStats(apple80211_stat_report *,apple80211_infra_specific_stats *) APPLE_KEXT_OVERRIDE;
#endif
    virtual SInt64 getWmeTxCounters(unsigned long long *) APPLE_KEXT_OVERRIDE;
    void setEnabledBySystem(bool);    // not an Apple vtable slot
    bool enabledBySystem(void);
    bool willRoam(ether_addr *,UInt);
    virtual void setPeerManagerLogFlag(UInt,UInt,UInt) APPLE_KEXT_OVERRIDE;
    virtual void setWoWEnabled(bool) APPLE_KEXT_OVERRIDE;
    virtual bool wowEnabled(void) APPLE_KEXT_OVERRIDE;
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,IOService *) APPLE_KEXT_OVERRIDE;
    virtual void releaseLinkQualityMonitor(IO80211Peer *) APPLE_KEXT_OVERRIDE;
    virtual int getAssocState(void) APPLE_KEXT_OVERRIDE;
    virtual void *getLQMSummary(apple80211_lqm_summary *) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_15_0
    virtual bool setLinkStateInternal(IO80211LinkState,uint,bool,uint
#else
    virtual IOReturn setLinkStateInternal(IO80211LinkState,uint,bool,uint
#endif
#if __IO80211_TARGET < __MAC_15_0
                                          ,apple80211_link_changed_event_data &
#endif
                                          );
    void setPoweredOnByUser(bool);       // not an Apple vtable slot (non-virtual)
    void setCurrentBssid(ether_addr *);  // (non-virtual)
    virtual void setCurrentApAddress(ether_addr *);                          // 463 import
    virtual void setWCL_ADVISORTY_INFO(apple80211_wcl_advisory_info *);      // 464
    virtual void *getWCL_TX_RX_LATENCY(apple80211_wcl_tx_rx_latency *);      // 465
    virtual void *createLQMData(void);                                       // 466 import
    
public:
#if __IO80211_TARGET >= __MAC_15_0
    void updateBSSIDProperty(ether_addr &, apple80211_channel &, bool);
    void updateSSIDProperty();
#endif
    char _data[0x120];
};

#endif /* IO80211InfraInterface_h */
