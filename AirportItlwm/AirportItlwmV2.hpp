//
//  AirportItlwmV2.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmV2_hpp
#define AirportItlwmV2_hpp

#include "Apple80211.h"

#include "IOKit/network/IOGatedOutputQueue.h"
#include <libkern/c++/OSString.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#include "ItlIwm.hpp"
#include "ItlIwx.hpp"
#include "ItlIwn.hpp"

#include "AirportItlwmEthernetInterface.hpp"

enum
{
    kPowerStateOff = 0,
    kPowerStateOn,
    kPowerStateCount
};

#define kWatchDogTimerPeriod 1000

extern "C" {
const char *convertApple80211IOCTLToString(signed int cmd);
}

class IOPCIEDeviceWrapper;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
#define AIRITL_LINK_DEFER 1
#endif
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
class AirportItlwmSkywalkInterface;
#endif

class AirportItlwm : public IO80211Controller {
    OSDeclareDefaultStructors(AirportItlwm)
#define IOCTL(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
} else { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
#define IOCTL_GET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
}
#define IOCTL_SET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCSA80211) { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
#define FUNC_IOCTL(REQ, DATA_TYPE) \
FUNC_IOCTL_GET(REQ, DATA_TYPE) \
FUNC_IOCTL_SET(REQ, DATA_TYPE)
#define FUNC_IOCTL_GET(REQ, DATA_TYPE) \
IOReturn get##REQ(OSObject *object, struct DATA_TYPE *data);
#define FUNC_IOCTL_SET(REQ, DATA_TYPE) \
IOReturn set##REQ(OSObject *object, struct DATA_TYPE *data);
    
public:
    virtual bool init(OSDictionary *properties) override;
    virtual void free() override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    IOReturn enable(IO80211SkywalkInterface *netif);   // V2 header makes this a non-virtual inline stub
    IOReturn disable(IO80211SkywalkInterface *netif);
    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setHardwareAddress(const IOEthernetAddress *addrP) override;
#endif
    virtual IOReturn setHardwareAddress(const void *addr, UInt32 addrBytes) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress* addrP) override;
#if __IO80211_TARGET >= __MAC_15_0
    IOReturn getHardwareAddressForInterface(IOEthernetAddress *addr);  // non-virtual: not an Apple vtable slot
#endif
    virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    virtual IOReturn setPromiscuousMode(IOEnetPromiscuousMode mode) override;
    virtual IOReturn setMulticastMode(IOEnetMulticastMode mode) override;
    virtual IOReturn setMulticastList(IOEthernetAddress* addr, UInt32 len) override;
    virtual UInt32 getFeatures() const override;
    virtual const OSString * newVendorString() const override;
    virtual const OSString * newModelString() const override;
    virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
    virtual bool createWorkQueue() override;
    virtual IONetworkInterface * createInterface() override;
    virtual bool configureInterface(IONetworkInterface *netif) override;
    virtual UInt32 outputPacket(mbuf_t, void * param) override;
    // Native queue callbacks use these without adding controller virtual slots.
    bool nativePacketOutputReady() const;
    void logNativeOutputState(const char *why) const; // diagnostic only
    UInt32 submitNativePacket(mbuf_t packet);
    static IOReturn nativeHooksGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeBootstrapGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeCoreEventGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeResetEventGated(OSObject *, void *, void *, void *, void *);
    IOReturn publishNativeCoreAvailability(bool, uint32_t, uint32_t);
    static IOReturn nativeQuiesceGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeEnableGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeEventGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeAbortJoinGated(OSObject *, void *, void *, void *, void *);
    IOReturn abortNativeJoin();
    static IOReturn nativeSetRadioPowerGated(OSObject *, void *, void *, void *, void *);
    bool nativeSystemPowerOn();
#ifdef __PRIVATE_SPI__
    virtual IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options) override;
    virtual IOReturn networkInterfaceNotification(
                        IONetworkInterface * interface,
                        uint32_t              type,
                        void *                  argument ) override;
#endif
    virtual bool setLinkStatus(
                               UInt32                  status,
                               const IONetworkMedium * activeMedium = 0,
                               UInt64                  speed        = 0,
                               OSData *                data         = 0) override;
    static IOReturn setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    void updateLQMIfChanged();
    void resetLQM();
    void resetLQM(bool force);
    
    static IOReturn tsleepHandler(OSObject* owner, void* arg0 = 0, void* arg1 = 0, void* arg2 = 0, void* arg3 = 0);
    static void eventHandler(struct ieee80211com *, int, void *);
    IOReturn enableAdapter(IONetworkInterface *netif);
    void disableAdapter(IONetworkInterface *netif);
    bool initCCLogs();
    
#if __IO80211_TARGET >= __MAC_15_0
    virtual IO80211WorkQueue *getWorkQueue() const override;
#else
    virtual IO80211WorkQueue *getWorkQueue() override;
#endif
    virtual bool requiresExplicitMBufRelease() override {
        return false;
    }
    virtual bool flowIdSupported() override {
        return false;
    }
    virtual SInt32 monitorModeSetEnabled(bool, UInt) override {
        return kIOReturnSuccess;
    }
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *queue, unsigned short *timeout) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**) override;
    
    virtual void *getFaultReporterFromDriver() override;             // 434: real Apple pure vtable slot
    virtual void *getLogger() const override { return driverLog; }   // 426: real Apple pure vtable slot — MUST be non-null (IO80211ControllerMonitor::init requires it)
    // The apple80211 ioctl/request helpers below are NOT Apple IO80211Controller vtable slots
    // (verified against the live KC). They are ordinary methods AirportItlwm calls directly.
    SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool);
#if __IO80211_TARGET >= __MAC_15_0
    SInt32 apple80211_ioctl_get(IO80211SkywalkInterface *,void *,bool,bool);
    SInt32 apple80211_ioctl_set(IO80211SkywalkInterface *,void *,bool,bool);
    SInt32 apple80211_ioctl_get(IO80211VirtualInterface *,void *,bool,bool);
    SInt32 apple80211_ioctl_set(IO80211VirtualInterface *,void *,bool,bool);
    bool useAppleRSNSupplicant(IO80211VirtualInterface *);
    SInt32 apple80211VirtualRequest(UInt,int,IO80211VirtualInterface *,void *);
    SInt32 setVirtualHardwareAddress(IO80211VirtualInterface *,ether_addr *);
#endif
    SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *);
    SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *);

    bool createMediumTables(const IONetworkMedium **primary);
    void releaseAll();
    void closeControllerTimerAdmission();
    bool holdControllerTimerOwner();
    bool quiesceControllerTimers();
    bool releaseControllerTimerReferences();
    bool controllerTimersClosing();
    bool armControllerWatchdog();
    struct ControllerTimerFrame {
        AirportItlwm *owner = NULL;
        void *thread = NULL;
        ControllerTimerFrame *next = NULL;
        IOTimerEventSource *source = NULL;
        bool rearm = false;
        ~ControllerTimerFrame();
    };
    bool beginControllerTimer(ControllerTimerFrame *, IOTimerEventSource *, bool scan);
    void endControllerTimer(ControllerTimerFrame *);
    IOLock *fControllerTimerLock;
    bool fControllerTimerClosing, fControllerTimerDraining, fControllerTimerOwnerHeld;
    ControllerTimerFrame *fControllerTimerFrames;
    IOTimerEventSource *fRetiredWatchdogTimer, *fRetiredScanTimer;

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
private:
    struct FailedStartCleanupFrame;
    bool directFailedStartCallerReady(IOService *, IOWorkLoop *, thread_t);
    bool cleanupFailedNativeStart(IOService *, AirportItlwmSkywalkInterface *, IOWorkLoop *, thread_t);
public:
    bool cleanup(IOService *provider, bool freeing = false, FailedStartCleanupFrame *authority = NULL);
#else
    bool cleanup(IOService *provider, bool freeing = false);
#endif
    static IOReturn nativeChildFinishCleanupGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeChildStopCompleteGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeChildCleanupReadyGated(OSObject *, void *, void *, void *, void *);
    void watchdogAction(IOTimerEventSource *timer);
    
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) override;
#if __IO80211_TARGET >= __MAC_15_0
    virtual SInt32 isCommandProhibited(int command) override {
#else
    virtual bool isCommandProhibited(int command) override {
#endif
//        if (!ml_at_interrupt_context())
//            XYLog("%s %s\n", __FUNCTION__, convertApple80211IOCTLToString(command));
        return false;
    };
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) override {
        XYLog("%s\n", __FUNCTION__);
        return 0;
    };
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getDRIVER_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getHARDWARE_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *interface,apple80211_capability_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCARD_CAPABILITIES((OSObject *)interface, data);
    }
    virtual IOReturn getPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getPOWER((OSObject *)interface, data);
    }
    virtual IOReturn setPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setPOWER((OSObject *)interface, data);
    }
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *interface,apple80211_debug_command *data) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    //scan
    static void fakeScanDone(OSObject *owner, IOTimerEventSource *sender);
    
    //-----------------------------------------------------------------------
    // Power management support.
    //-----------------------------------------------------------------------
    virtual IOReturn registerWithPolicyMaker( IOService * policyMaker ) override;
    virtual IOReturn setPowerState( unsigned long powerStateOrdinal,
                                    IOService *   policyMaker) override;
    virtual IOReturn setWakeOnMagicPacket( bool active ) override;
    void setPowerStateOff(void);
    void setPowerStateOn(void);
    bool unregistPM(bool permanent = true);
    struct PowerCallFrame { void *thread; PowerCallFrame *next; };
    void beginPowerCall(PowerCallFrame *);
    void endPowerCall(PowerCallFrame *);
    bool initPCIPowerManagment(IOPCIDevice *provider);

    FUNC_IOCTL_GET(CARD_CAPABILITIES, apple80211_capability_data)
    FUNC_IOCTL(POWER, apple80211_power_data)
    FUNC_IOCTL_GET(DRIVER_VERSION, apple80211_version_data)
    FUNC_IOCTL_GET(HARDWARE_VERSION, apple80211_version_data)
    FUNC_IOCTL(COUNTRY_CODE, apple80211_country_code_data)
    
public:
    IOInterruptEventSource* fInterrupt;
    IOTimerEventSource *watchdogTimer;
    IOPCIDevice *pciNub;
    IONetworkStats *fpNetStats;
#ifndef AIRITL_ENABLE_BSDIF
    IONetworkStats fNativeNetStats;
#endif
    AirportItlwmEthernetInterface *bsdInterface;
    IO80211SkywalkInterface *fNetIf;
    IOWorkLoop *fWatchdogWorkLoop;
    ItlHalService *fHalService;
    UInt64 fLastReportedLQM;
    
    // Each flag records an acquisition that cleanup() must undo exactly once.
    bool fControllerStarted = false;
    bool fNativeInitComplete = false;
    bool fNativeStartLifetimeHeld = false;
    enum class NativeStartPhase { NotAttempted, InProgress, Succeeded, Failed };
    NativeStartPhase fNativeStartPhase = NativeStartPhase::NotAttempted;
    IOPCIEDeviceWrapper *fWrapperProvider = NULL;
    bool fControllerOwnerClaimed = false;
    OSObject *fTimerProbe;
    bool fHalAttached = false;
    bool fHalAttachAttempted = false;
    bool fChildTerminationRequested = false;
    bool fStartInProgress = false;
    bool fInterfaceAttached = false;
    bool fInterfacePublished = false;
    bool fControllerInterfaceAttached = false;
    bool fEthernetAttached = false;
    bool fCommandGateAcquired = false;
    bool fCommandGateAdded = false;
    bool fSnapshotsPipeStarted = false;
    bool fScanSourceAdded = false;
    bool fWatchdogTimerAdded = false;
    bool fCleanupInProgress = false;
    bool fStopping = false;
    bool fNativeHooksInstalled = false;
    bool fNativeBootstrapDispatched;
    bool fNativeResetActive;
    struct ieee80211_native_reset_event fNativeReset;
    bool fNativeAvailabilityDispatched;
    bool fNativeCoreAvailable;
    uint32_t fNativeCoreReason;
    uint32_t fNativeCoreFlags;

    //IO80211
    uint8_t power_state;
    uint64_t fNativePowerSequence; // Main-gate radio-operation ownership.
    struct ieee80211_node *fNextNodeToSend;
    bool fScanResultWrapping;
    IOTimerEventSource *scanSource;
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    UInt64 currentSpeed;
    UInt32 currentStatus;
    bool disassocIsVoluntary;
    char geo_location_cc[3];
    
    //pm
    thread_call_t powerOnThreadCall;
    thread_call_t powerOffThreadCall;
    IOLock *fPowerCallLock;
    bool fPowerCallAdmission, fPowerCallStopping, fPowerCallDraining, fPowerCallOwnerHeld;
    PowerCallFrame *fPowerCallFrames;
    unsigned fPowerCallOutstanding;
    UInt32 pmPowerState;
    IOService *pmPolicyMaker;
    UInt8 pmPCICapPtr;
    bool magicPacketEnabled;
    bool magicPacketSupported;
    
    //AWDL
    uint8_t *syncFrameTemplate;
    uint32_t syncFrameTemplateLength;
    uint8_t awdlBSSID[6];
    uint32_t awdlSyncState;
    uint32_t awdlElectionId;
    uint32_t awdlPresenceMode;
    uint16_t awdlMasterChannel;
    uint16_t awdlSecondaryMasterChannel;
    uint8_t *roamProfile;
    struct apple80211_btc_profiles_data *btcProfile;
    struct apple80211_btc_config_data btcConfig;
    uint32_t btcMode;
    uint32_t btcOptions;
    bool awdlSyncEnable;
    
    CCPipe *driverLogPipe;
    CCPipe *driverDataPathPipe;
    CCPipe *driverSnapshotsPipe;
    
    OSObject *driverFaultReporter;
    CCLogStream *driverLog;
    enum class FirstNativeInitPhase : UInt8 { NotAttempted = 0, InProgress, Failed, Succeeded };
    FirstNativeInitPhase fFirstNativeInitPhase = FirstNativeInitPhase::NotAttempted;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    UInt32 fLatestRequestedPowerState;
#endif
#ifdef AIRITL_LINK_DEFER
public:
    // Native link setters must run in the main gate but not on the main
    // workloop thread. Legacy link changes arrive on that thread, so they are
    // published from a thread call that enters the gate. Latest state wins.
    void runDeferredLinkState();
    // Closes deferred publication for good and waits until no worker is queued
    // or running. Every path that stops the native interface calls it first.
    bool quiesceLinkStateCall();
    // Ordering rule between the two link publishers: a native WCL link
    // publication supersedes any legacy state requested before it. Bumped
    // only inside the main gate, immediately before a native publication.
    // Only a native publication of the opposite state supersedes; a native
    // publication that agrees with the legacy state (its own reaction to it)
    // does not.
    void noteNativeLinkPublication(bool up) {
        if (up != fNativeLinkLastUp || !fNativeLinkPublished) ++fNativeLinkEpoch;
        fNativeLinkLastUp = up; fNativeLinkPublished = true;
    }
    bool nativeLinkSupersedes(UInt32 epoch, IO80211LinkState state) const {
        return fNativeLinkEpoch != epoch &&
               fNativeLinkLastUp != (state == kIO80211NetworkLinkUp);
    }
    static IOReturn deferredLinkGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
private:
    bool requestLinkState(IO80211LinkState state, UInt32 reason);
    volatile UInt32 fNativeLinkEpoch = 0;
    bool fNativeLinkLastUp = false;
    bool fNativeLinkPublished = false;
    UInt32 fLinkStateEpoch = 0;      // native epoch observed when the mailbox state was requested
    thread_call_t fLinkStateCall = NULL;
    IOLock *fLinkStateLock = NULL;
    void *fLinkStateWorker = NULL;
    IO80211LinkState fLinkStateWanted = kIO80211NetworkLinkDown;
    UInt32 fLinkStateReason = 0;
    unsigned fLinkStateOutstanding = 0;
    bool fLinkStatePending = false;
    bool fLinkStateQueued = false;   // one invocation submitted, not yet started
    bool fLinkStateRunning = false;  // one invocation publishing; at most one exists
    bool fLinkStateStopping = false;
#endif
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
private:
    FailedStartCleanupFrame *fFailedStartCleanup = NULL;
#endif

};

#endif /* AirportItlwmV2_hpp */
