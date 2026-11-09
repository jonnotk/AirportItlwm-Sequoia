//
//  IO80211SkywalkInterface.h
//  IO80211Family
//
//  Created by 钟先耀 on 2019/10/18.
//  Copyright © 2019 钟先耀. All rights reserved.
//

#ifndef _IO80211SKYWALK_H
#define _IO80211SKYWALK_H

#include <Availability.h>
#include "IOSkywalkEthernetInterface.h"

// This is necessary, because even the latest Xcode does not support properly targeting 11.0.
#ifndef __IO80211_TARGET
#error "Please define __IO80211_TARGET to the requested version"
#endif

class TxSubmissionDequeueStats;
class TxCompletionEnqueueStats;
class IO80211NetworkPacket;
class PacketSkywalkScratch;
#ifndef IO80211_FLOW_QUEUE_HASH_DECLARED
#define IO80211_FLOW_QUEUE_HASH_DECLARED
#if __IO80211_TARGET >= __MAC_15_0
struct IO80211FlowQueueHash {
    UInt64 value;
};
#else
typedef UInt64 IO80211FlowQueueHash;
#endif
#endif
class IO80211Peer;
class CCPipe;
class IO80211APIUserClient;
// Apple's real type is a 4-byte enum (WME access category). It was previously only an
// incomplete forward decl in vtable-slot signatures; it must be complete to DEFINE
// an override of getTxSubQueue(apple80211_wme_ac) by value. 4-byte wrapper == enum ABI; the
// mangled name ('17apple80211_wme_ac') is identical to the KC symbol, so vtable binding is unchanged.
struct apple80211_wme_ac { uint32_t ac; };
struct apple80211_interface_availability;
struct apple80211_cca_report;
struct apple80211_stat_report;
struct apple80211_chip_counters_tx;
struct apple80211_chip_counters_rx;
struct apple80211_chip_error_counters_tx;
struct apple80211_ManagementInformationBasedot11_counters;
struct apple80211_lteCoex_report;
struct apple80211_frame_counters;
struct userPrintCtx;
struct apple80211_lqm_summary;
struct apple80211_infra_specific_stats;

struct TxPacketRequest {
    uint16_t    unk1;       // 0
    uint16_t    t;       // 2
    uint16_t    mU;       // 4
    uint16_t    mM;       // 6
    uint16_t    pkt_cnt;
    uint16_t    unk2;
    uint16_t    unk3;
    uint16_t    unk4;
    uint32_t    pad;
    mbuf_t      bufs[8];    // 18
    uint32_t    reqTx;
};

static_assert(sizeof(struct TxPacketRequest) == 0x60, "TxPacketRequest size error");

class IO80211PeerManager;
class IO80211Controller;
struct apple80211_data_path_interface_stats;
struct apple80211_data_path_peer_stats;
struct apple80211_latency_all_ac;

class IO80211SkywalkInterface : public IOSkywalkEthernetInterface {
    OSDeclareAbstractStructors(IO80211SkywalkInterface)

public:
    
    virtual bool init() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
        IOService *   whatDevice ) APPLE_KEXT_OVERRIDE;
    virtual unsigned long maxCapabilityForDomainState( IOPMPowerFlags domainState ) APPLE_KEXT_OVERRIDE;
    virtual unsigned long initialPowerStateForDomainState( IOPMPowerFlags domainState ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enable(UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(UInt) APPLE_KEXT_OVERRIDE;
    virtual SInt32 initBSDInterfaceParameters(ifnet_init_eparams *,sockaddr_dl **) APPLE_KEXT_OVERRIDE;
    virtual bool prepareBSDInterface(ifnet_t, UInt) APPLE_KEXT_OVERRIDE;
    virtual IOReturn processBSDCommand(ifnet_t, UInt, void *) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;
    virtual SInt32 setRunningState(bool) APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleChosenMedia(UInt) APPLE_KEXT_OVERRIDE;
    virtual void *getSupportedMediaArray(UInt *,UInt *) APPLE_KEXT_OVERRIDE;
    virtual UInt32 getFeatureFlags(void) APPLE_KEXT_OVERRIDE;
    virtual const char *classNameOverride(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousModeEnable(bool, UInt) APPLE_KEXT_OVERRIDE;
    virtual void *createPeerManager(void);
    virtual void *createPeer(const unsigned char *, IO80211PeerManager *);                 // 355
    virtual void postMessage(UInt,void *,unsigned long,bool);
    virtual IOReturn reportDataPathEvents(UInt,void *,unsigned long,bool);
    virtual IOReturn recordOutputPackets(TxSubmissionDequeueStats *,TxSubmissionDequeueStats *);
    virtual IOReturn recordOutputPacket(apple80211_wme_ac,int,int);
    virtual void logTxPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,apple80211_wme_ac,bool);
    virtual void logTxCompletionPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,int,UInt,bool
#if __IO80211_TARGET >= __MAC_15_0
                                      ,bool
#endif
                                      );
    virtual IOReturn recordCompletionPackets(TxCompletionEnqueueStats *,TxCompletionEnqueueStats *);
    virtual IOReturn inputPacket(IO80211NetworkPacket *,packet_info_tag *,ether_header *,bool *
#if __IO80211_TARGET >= __MAC_15_0
                                 ,bool
#endif
                                 );
    virtual IOReturn forwardInfraRelayPackets(IO80211NetworkPacket*, ether_header*);
    virtual void logSkywalkTxReqPacket(IO80211NetworkPacket *,PacketSkywalkScratch *,unsigned char *,apple80211_wme_ac,bool);
    virtual SInt64 pendingPackets(unsigned char);
    virtual SInt64 packetSpace(unsigned char);
    virtual bool isChipInterfaceReady(void);
    virtual bool isDebounceOnGoing(void);
    virtual bool setLinkState(IO80211LinkState,UInt,bool debounceTimeout = 30,UInt code = 0);
    virtual IO80211LinkState linkState(void);
    virtual void setScanningState(UInt,bool,apple80211_scan_data *,int);
    virtual void setDataPathState(bool);
    virtual void *getScanManager(void);
    virtual IO80211Controller *getController(void);                                          // 375
    virtual void updateLinkParameters(apple80211_interface_availability *);
    virtual void updateInterfaceCoexRiskPct(unsigned long long);
    virtual void setLQM(unsigned long long);
    virtual void updateLinkStatus(void);
    virtual void updateLinkStatusGated(void);
    virtual void setInterfaceExtendedCCA(apple80211_channel,apple80211_cca_report *);
    virtual void setInterfaceCCA(apple80211_channel,int);
    virtual void setInterfaceNF(apple80211_channel,long long);
    virtual void setInterfaceOFDMDesense(apple80211_channel,long long);
    virtual void removePacketQueue(IO80211FlowQueueHash *);
    virtual void setDebugFlags(unsigned long long,UInt);
    virtual SInt64 debugFlags(void);
    virtual void setInterfaceChipCounters(apple80211_stat_report *,apple80211_chip_counters_tx *,apple80211_chip_error_counters_tx *,apple80211_chip_counters_rx *);
    virtual void setInterfaceMIBdot11(apple80211_stat_report *,apple80211_ManagementInformationBasedot11_counters *);
    virtual void setFrameStats(apple80211_stat_report *,apple80211_frame_counters *);
#if __IO80211_TARGET >= __MAC_14_4
    virtual void setInfraSpecificFrameStats(apple80211_stat_report *,apple80211_infra_specific_stats *);
#endif
    virtual SInt64 getWmeTxCounters(unsigned long long *);
    void setEnabledBySystem(bool);          // not an Apple IO80211SkywalkInterface vtable slot
    bool enabledBySystem(void);             // not an Apple vtable slot
    bool willRoam(ether_addr *,UInt);       // not an Apple vtable slot
    virtual void setPeerManagerLogFlag(UInt,UInt,UInt);
    virtual void setWoWEnabled(bool);
    virtual bool wowEnabled(void);
    virtual void printDataPath(userPrintCtx *);
    virtual UInt32 getDataQueueDepth(void);                                                  // 397
    virtual bool findOrCreateFlowQueue(IO80211FlowQueueHash);
    virtual UInt64 findOrCreateFlowQueueWithCache(IO80211FlowQueueHash,bool *);
    virtual UInt64 findExistingFlowQueue(IO80211FlowQueueHash);
    virtual void removePacketQueue(IO80211FlowQueueHash const*);
    virtual void flushPacketQueues(void);
    virtual void cachePeer(ether_addr *,UInt *);
    virtual bool shouldLog(unsigned long long);
    virtual void vlogDebug(unsigned long long,char const*,va_list);
    virtual void vlogDebugBPF(unsigned long long,char const*,va_list);
    virtual UInt64 createLinkQualityMonitor(IO80211Peer *,IOService *);
    virtual void releaseLinkQualityMonitor(IO80211Peer *);
    virtual void *getP2PSkywalkPeerMgr(void);
#if __IO80211_TARGET >= __MAC_15_0
    virtual SInt32 isCommandProhibited(int);
#else
    virtual bool isCommandProhibited(int);
#endif
    virtual void *findPeer(ether_addr &);                                                    // 411
    virtual void setNotificationProperty(OSSymbol const*,OSObject const*);
    virtual void *getWorkerMatchingDict(OSString *);
#if __IO80211_TARGET >= __MAC_15_0
    virtual bool init(IOService *, ether_addr *);
#else
    virtual bool init(IOService *);
#endif
    virtual bool isInterfaceEnabled(void);
    virtual ether_addr *getSelfMacAddr(void);
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setMacAddress(ether_addr &) = 0; // 417, native caller checks EAX
#else
    virtual void setSelfMacAddr(ether_addr *) = 0;   // 417 (pure; concrete driver implements)
#endif
    virtual void *getPacketPool(OSString *);
    virtual void *getLogger(void)
#if __IO80211_TARGET >= __MAC_15_0
    const
#endif
    ;
    virtual IOReturn handleSIOCSIFADDR(void);
    virtual IOReturn debugHandler(apple80211_debug_command *);
    virtual void statsDump(void);
    virtual void powerOnNotification(void);
    virtual void powerOffNotification(void);
    virtual UInt64 getTxQueueDepth(void);
    virtual UInt64 getRxQueueCapacity(void);
    virtual void updateRxCounter(unsigned long long);
    virtual void *getMultiCastQueue(void);
    void *getCurrentBssid(void);            // not an Apple vtable slot
    virtual int getAssocState(void);
    virtual void notifyQueueState(apple80211_wme_ac,unsigned short);
    virtual int getTxHeadroom(void);
    virtual void *getRxCompQueue(void);
    virtual void *getTxCompQueue(void);
    virtual void *getTxSubQueue(apple80211_wme_ac);
    virtual void *getTxPacketPool(void);
    virtual void *getRxPacketPool(void);
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn enableDatapath(void);
    virtual IOReturn disableDatapath(void);
#else
    virtual void enableDatapath(void);
    virtual void disableDatapath(void);
#endif
    virtual int getNumTxQueues(void);
    virtual void *getLQMSummary(apple80211_lqm_summary *);
    virtual int getEventPipeSize(void);
    virtual UInt64 createEventPipe(IO80211APIUserClient *);
    virtual void destroyEventPipe(IO80211APIUserClient *);
    virtual IOReturn setUserBufferInfo(IOMemoryDescriptor *, unsigned long long);            // 444
    virtual void postMessageIOUC(char const*,UInt,void *,unsigned long);
    virtual bool isIOUCPipeOpened(void);
    virtual void *getRingMD(IO80211APIUserClient *,unsigned long long);
    // Apple vtable tail (slots 448-461) — peer/datapath-stats/latency block.
    virtual IOReturn attachPeer(ether_addr *);                                               // 448
    virtual IOReturn detachPeer(ether_addr *);                                               // 449
    virtual void setDebugTrafficReport(bool);                                                // 450
    virtual IOReturn getDataPathInterfaceStats(apple80211_data_path_interface_stats *);      // 451
    virtual IOReturn getDataPathPeerStats(apple80211_data_path_peer_stats *);                // 452
    virtual IOReturn getLastQueuePacketTime(ether_addr *);                                   // 453
    virtual IOReturn getLastRxUnicastLinkActivityTime(ether_addr *);                         // 454
    virtual void updateInterfaceDataStats(apple80211_data_path_interface_stats *);           // 455
    virtual void updatePeerDataStats(apple80211_data_path_peer_stats *);                     // 456
    virtual void logTxLatency(unsigned char *, UInt, unsigned long long);                    // 457
    virtual void logRxLatency(UInt, unsigned long long);                                     // 458
    virtual void getNClearTxRxLatency(apple80211_latency_all_ac *, apple80211_latency_all_ac *); // 459
    virtual IOReturn getLastTxTimeStamp(unsigned long long &);                               // 460
    virtual IOReturn getLastRxTimeStamp(unsigned long long &);                               // 461

public:
    OSString *setInterfaceRole(UInt role);
    void *setInterfaceId(UInt id);
    int getInterfaceRole();
    
public:
    char _data[0x118];
};

#endif /* _IO80211SKYWALK_H */
