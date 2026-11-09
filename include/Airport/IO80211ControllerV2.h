//
//  IOSkywalkInterface.h
//  itlwm
//
//  Created by qcwap on 2023/6/7.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef _IO80211CONTROLLER_H
#define _IO80211CONTROLLER_H

#if defined(KERNEL) && defined(__cplusplus)

#include <Availability.h>
#include <libkern/version.h>

// This is necessary, because even the latest Xcode does not support properly targeting 11.0.
#ifndef __IO80211_TARGET
#error "Please define __IO80211_TARGET to the requested version"
#endif

#if VERSION_MAJOR > 8
#define _MODERN_BPF
#endif

#include <sys/kpi_mbuf.h>

#include <IOKit/network/IOEthernetController.h>

#include <sys/param.h>
#include <net/bpf.h>

#include "apple80211_ioctl.h"
#include "IO80211SkywalkInterface.h"
#include "IO80211WorkLoop.h"
#include "IO80211WorkQueue.h"
#include "CCStream.h"
#include "CCDataPipe.h"
#include "CCLogPipe.h"
#include "CCLogStream.h"

#define AUTH_TIMEOUT            15    // seconds

/*! @enum LinkSpeed.
 @abstract ???.
 @discussion ???.
 @constant LINK_SPEED_80211A 54 Mbps
 @constant LINK_SPEED_80211B 11 Mbps.
 @constant LINK_SPEED_80211G 54 Mbps.
 */
enum {
    LINK_SPEED_80211A    = 54000000ul,        // 54 Mbps
    LINK_SPEED_80211B    = 11000000ul,        // 11 Mbps
    LINK_SPEED_80211G    = 54000000ul,        // 54 Mbps
    LINK_SPEED_80211N    = 300000000ul,        // 300 Mbps (MCS index 15, 400ns GI, 40 MHz channel)
};

enum IO80211CountryCodeOp
{
    kIO80211CountryCodeReset,                // Reset country code to world wide default, and start
    // searching for 802.11d beacon
};
typedef enum IO80211CountryCodeOp IO80211CountryCodeOp;

enum IO80211SystemPowerState
{
    kIO80211SystemPowerStateUnknown,
    kIO80211SystemPowerStateAwake,
    kIO80211SystemPowerStateSleeping,
};
typedef enum IO80211SystemPowerState IO80211SystemPowerState;

enum IO80211FeatureCode
{
    kIO80211Feature80211n = 1,
};
typedef enum IO80211FeatureCode IO80211FeatureCode;


class IOSkywalkInterface;
class IO80211ScanManager;

enum scanSource
{
    SOURCE_1,
};

enum joinStatus
{
    STATUS_1,
};

class IO80211Controller;
class IO80211Interface;
class IO82110WorkLoop;
class IO80211VirtualInterface;
class IO80211ControllerMonitor;
class CCLogPipe;
class CCIOReporterLogStream;
class CCLogStream;
class IO80211VirtualInterface;
class IO80211RangingManager;
class IO80211FlowQueue;
class IO80211FlowQueueLegacy;
class FlowIdMetadata;
class IOReporter;
class IO80211InfraInterface;
extern void IO80211VirtualInterfaceNamerRetain();


struct apple80211_hostap_state;

struct apple80211_awdl_sync_channel_sequence;
struct ieee80211_ht_capability_ie;
struct apple80211_channel_switch_announcement;
struct apple80211_beacon_period_data;
struct apple80211_power_debug_sub_info;
struct apple80211_stat_report;
struct apple80211_frame_counters;
struct apple80211_leaky_ap_event;
struct apple80211_chip_stats;
struct apple80211_extended_stats;
struct apple80211_ampdu_stat_report;
struct apple80211_btCoex_report;
struct apple80211_cca_report;
struct apple80211_platform_config;
class CCPipe;
struct apple80211_lteCoex_report;

//typedef int scanSource;
//typedef int joinStatus;
//typedef int CCStreamLogLevel;
typedef IOReturn (*IOCTL_FUNC)(IO80211Controller*, IO80211Interface*, IO80211VirtualInterface*, apple80211req*, bool);
extern IOCTL_FUNC gGetHandlerTable[];
extern IOCTL_FUNC gSetHandlerTable[];

class IO80211InterfaceAVCAdvisory;

class IO80211Controller : public IOEthernetController {
    OSDeclareAbstractStructors(IO80211Controller)

public:

    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool init(OSDictionary *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *) APPLE_KEXT_OVERRIDE;
    virtual IOWorkLoop* getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
    virtual const char* stringFromReturn(int) APPLE_KEXT_OVERRIDE;
    virtual int errnoFromReturn(int) APPLE_KEXT_OVERRIDE;
    virtual UInt32 getFeatures() const APPLE_KEXT_OVERRIDE;
    virtual const OSString * newVendorString() const APPLE_KEXT_OVERRIDE;
    virtual const OSString * newModelString() const APPLE_KEXT_OVERRIDE;
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setHardwareAddress(const IOEthernetAddress * addrP) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
    // === IO80211Controller-introduced virtuals — EXACT Apple Sequoia vtable order (slots 396-465).
    // Reconstructed from live BootKernelExtensions.kc (docs/.../vtable-analysis/). Order is ABI-critical:
    // Apple's IO80211Family dispatches into this controller by these slot indices. DO NOT reorder.
#if __IO80211_TARGET >= __MAC_15_0
    virtual SInt32 isCommandProhibited(int) = 0;
#else
    virtual bool isCommandProhibited(int) = 0;
#endif                                                   // 396 pure
    virtual bool createWorkQueue();                                                              // 397
    virtual void debugStateInit();                                                               // 398 import
#if __IO80211_TARGET >= __MAC_15_0
    virtual IO80211WorkQueue *getWorkQueue() const;
#else
    virtual IO80211WorkQueue *getWorkQueue();
#endif                                                    // 399
    virtual void requestPacketTx(void*, UInt);                                                   // 400
    virtual IOCommandGate *getIO80211CommandGate() const;                                        // 401
    virtual IO80211SkywalkInterface* getPrimarySkywalkInterface(void);                           // 402
    virtual int bpfOutputPacket(OSObject *,UInt,mbuf_t m);                                       // 403
    virtual SInt32 monitorModeSetEnabled(bool, UInt);                                            // 404
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) = 0;  // 405 pure
    virtual UInt32 hardwareOutputQueueDepth();                                                   // 406
    virtual SInt32 performCountryCodeOperation(IO80211CountryCodeOp);                            // 407
    virtual void dataLinkLayerAttachComplete();                                                  // 408
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) = 0;                                 // 409 pure
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;        // 410
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *) = 0;      // 411
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *) = 0;  // 412
    virtual IOReturn getPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;                   // 413
    virtual IOReturn setPOWER(IO80211SkywalkInterface *,apple80211_power_data *) = 0;                   // 414
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;     // 415
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *) = 0;     // 416
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *,apple80211_debug_command *) = 0;       // 417
    virtual IOReturn getPLATFORM_CONFIG(IO80211SkywalkInterface *,apple80211_platform_config *);        // 418 import
    virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *);                            // 419
    virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *);                           // 420
    virtual bool requiresExplicitMBufRelease();                                                  // 421
    virtual bool flowIdSupported() { return false; }                                             // 422
    virtual IO80211FlowQueueLegacy* requestFlowQueue(FlowIdMetadata const*);                     // 423
    virtual void releaseFlowQueue(IO80211FlowQueue *);                                           // 424
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**);                                      // 425
    virtual void *getLogger() const = 0;                                                         // 426 pure (AirportItlwm impl)
    virtual void enableFeatureForLoggingFlags(unsigned long long) {};                            // 427
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *, unsigned short *) { return kIOReturnIOError; };  // 428
    virtual IOReturn enablePacketTimestamping(void) { return kIOReturnUnsupported; }             // 429
    virtual IOReturn disablePacketTimestamping(void) { return kIOReturnUnsupported; }            // 430
    virtual UInt getPacketTSCounter();                                                           // 431
    virtual void *getDriverTextLog();                                                            // 432
    virtual UInt32 selfDiagnosticsReport(int,char const*,UInt);                                  // 433
    virtual void *getFaultReporterFromDriver();                                                  // 434 (AirportItlwm impl)
    virtual void *allocIO80211RecursiveLock();                                                   // 435 import
    virtual UInt32 getDataQueueDepth(OSObject *);                                                // 436
    virtual bool wasDynSARInFailSafeMode(void) { return false; }                                 // 437
    virtual void updateAdvisoryScoresIfNeed(void);                                               // 438
    virtual UInt64 getAVCAdvisoryInfo(IO80211InterfaceAVCAdvisory *);                            // 439
    virtual unsigned int getActionFramePoolCapacity();                                           // 440 import
    virtual void *getPostOffice();                                                               // 441 import
    virtual void *CreatePostOffice();                                                            // 442 import
    virtual bool attachInterface(OSObject *,IOService *);                                        // 443
    virtual void detachInterface(OSObject *,bool);                                               // 444
    virtual IO80211VirtualInterface* createVirtualInterface(ether_addr *,UInt);                  // 445
    virtual bool attachVirtualInterface(IO80211VirtualInterface **,ether_addr *,UInt,bool);      // 446
    virtual bool detachVirtualInterface(IO80211VirtualInterface *,bool);                         // 447
    OSMetaClassDeclareReservedUnused( IO80211Controller,  0);                                    // 448
    OSMetaClassDeclareReservedUnused( IO80211Controller,  1);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  2);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  3);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  4);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  5);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  6);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  7);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  8);
    OSMetaClassDeclareReservedUnused( IO80211Controller,  9);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 10);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 11);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 12);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 13);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 14);
    OSMetaClassDeclareReservedUnused( IO80211Controller, 15);                                    // 463
public:
    virtual IOReturn setMulticastList(ether_addr const*, UInt);                                  // 464

    // === Non-virtual helpers: NOT in Apple's IO80211Controller vtable.
    // Kept as ordinary methods so they occupy NO vtable slot; AirportItlwm calls them directly.
    IOReturn getHardwareAddressForInterface(IOEthernetAddress *);
    bool useAppleRSNSupplicant(IO80211VirtualInterface *);
    // Inline stub bodies: these are NOT Apple IO80211Controller symbols, so AirportItlwm's
    // `super::X` calls must resolve to a local definition (else the kext fails to prelink at boot).
    SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool) { return kIOReturnUnsupported; }
    SInt32 apple80211VirtualRequest(UInt,int,IO80211VirtualInterface *,void *) { return kIOReturnUnsupported; }
    SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *) { return kIOReturnUnsupported; }
    SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *) { return kIOReturnUnsupported; }
    SInt32 setVirtualHardwareAddress(IO80211VirtualInterface *,ether_addr *) { return kIOReturnUnsupported; }
    SInt32 apple80211_ioctl_get(IO80211SkywalkInterface *,void *, bool, bool) { return kIOReturnUnsupported; }
    SInt32 apple80211_ioctl_set(IO80211SkywalkInterface *,void *, bool, bool) { return kIOReturnUnsupported; }
    SInt32 apple80211_ioctl_get(IO80211VirtualInterface *,void *,bool,bool) { return kIOReturnUnsupported; }
    SInt32 apple80211_ioctl_set(IO80211VirtualInterface *,void *,bool,bool) { return kIOReturnUnsupported; }
    IOReturn enable(IO80211SkywalkInterface *) { return kIOReturnSuccess; }
    IOReturn disable(IO80211SkywalkInterface *) { return kIOReturnSuccess; }
    bool isAssociatedToMovingNetwork(void) { return false; }

protected:
    uint8_t  filler[0x128];
};

#endif /* defined(KERNEL) && defined(__cplusplus) */

#endif /* !_IO80211CONTROLLER_H */
