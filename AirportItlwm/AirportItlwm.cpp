/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include "AirportItlwm.hpp"

#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>
#include <HAL/AiritlDebug.h>

// Gated debug logging (see include/HAL/AiritlDebug.h). This target's own
// definition: the pre-Sonoma AirportItlwm targets compile neither
// AirportItlwmV2.cpp nor itlwm.cpp, but share HAL files that use AIRITL_DBG.
extern "C" {
int airitl_debug_log = 0;
}

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkLoop *_fWorkloop;
IOCommandGate *_fCommandGate;

#define ITL_CONTROLLER_CLASS AirportItlwm
#define ITL_CONTROLLER_HAS_SCAN 1
#include <HAL/ItlControllerTimers.inc>
#undef ITL_CONTROLLER_HAS_SCAN
#undef ITL_CONTROLLER_CLASS

bool AirportItlwm::init(OSDictionary *properties)
{
    bool ret = super::init(properties);
    awdlSyncEnable = true;
    power_state = 0;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    fPowerCallLock = ret ? IOLockAlloc() : NULL;
    fPowerCallAdmission = fPowerCallStopping = fPowerCallDraining = fPowerCallOwnerHeld = false;
    fPowerCallFrames = NULL;
    fPowerCallOutstanding = 0;
    powerOffThreadCall = powerOnThreadCall = NULL;
    pmPolicyMaker = NULL;
    pmPowerState = kPowerStateOn;
    fControllerTimerLock = ret ? IOLockAlloc() : NULL;
    fControllerTimerClosing = fControllerTimerDraining = fControllerTimerOwnerHeld = false;
    fControllerTimerFrames = NULL;
    fRetiredWatchdogTimer = fRetiredScanTimer = NULL;
    fWatchdogTimerAdded = fScanSourceAdded = false;
    watchdogTimer = NULL;
    scanSource = NULL;
    fLegacyCleanupInProgress = fLegacyStartInProgress = fLegacyBaseStarted = fLegacyBaseStartInProgress = false;
    fLegacyHalAttachAttempted = fLegacyHalAttached = fLegacyHalAttachInProgress = false;
    fLegacyInterfaceAttached = fLegacyInterfaceAttachInProgress = false;
    fLegacyChildTerminationRequested = fLegacyEthernetAttached = fLegacyCommandGateAdded = false;
    return ret && fPowerCallLock && fControllerTimerLock;
}

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    u16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    u16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    bool isMatch = false;
    super::probe(provider, score);
    UInt8 msiCap = 0;
    UInt8 msixCap = 0;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device)
        return NULL;
    if (ItlIwx::iwx_match(device)) {
        isMatch = true;
        fHalService = new ItlIwx;
    }
    if (!isMatch && ItlIwm::iwm_match(device)) {
        isMatch = true;
        fHalService = new ItlIwm;
    }
    if (!isMatch && ItlIwn::iwn_match(device)) {
        isMatch = true;
        fHalService = new ItlIwn;
    }
    if (isMatch && !fHalService)
        return NULL;
    if (isMatch) {
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
        if (msixCap)
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
        if (msiCap)
            pciMsiSetEnable(device, msiCap, 1);
        if (!msiCap && !msixCap) {
            XYLog("%s No MSI cap\n", __FUNCTION__);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        return this;
    }
    return NULL;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    IOLockLock(fControllerTimerLock);
    fLegacyEthernetAttached = true;
    IOLockUnlock(fControllerTimerLock);
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmInterface *netif = new AirportItlwmInterface;
    if (!netif)
        return NULL;
    if (!netif->init(this, fHalService)) {
        netif->release();
        return NULL;
    }
    return netif;
}

void AirportItlwm::associateSSID(uint8_t *ssid, uint32_t ssid_len, const struct ether_addr &bssid, uint32_t authtype_lower, uint32_t authtype_upper, uint8_t *key, uint32_t key_len, int key_index) 
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    
    struct ieee80211_wpaparams	 wpa;
    struct ieee80211_nwkey		 nwkey;
    bzero(&wpa, sizeof(wpa));
    bzero(&nwkey, sizeof(nwkey));
    
    memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
    memcpy(ic->ic_des_essid, ssid, ssid_len);
    ic->ic_des_esslen = ssid_len;
    
    bool is_zero = true;
    for (int i = 0; i < IEEE80211_ADDR_LEN; i++)
    is_zero &= bssid.octet[i] == 0;
    
    if (!is_zero) {
        IEEE80211_ADDR_COPY(ic->ic_des_bssid, bssid.octet);
        ic->ic_flags |= IEEE80211_F_DESBSSID;
    }
    else {
        memset(ic->ic_des_bssid, 0, IEEE80211_ADDR_LEN);	
        ic->ic_flags &= ~IEEE80211_F_DESBSSID;
    }

    // AUTHTYPE_WPA3_SAE AUTHTYPE_WPA3_FT_SAE
    // we don't really support WPA3, but we have announced we support WPA3 in card capability function. so we fake it as WPA2 to support some WPA2/WPA3 mix wifi connection.
    if (authtype_upper == APPLE80211_AUTHTYPE_WPA3_SAE || authtype_upper == APPLE80211_AUTHTYPE_WPA3_FT_SAE) {
        wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        authtype_upper |= APPLE80211_AUTHTYPE_WPA2_PSK;// hack
    }
    // AUTHTYPE_WPA3_ENTERPRISE AUTHTYPE_WPA3_FT_ENTERPRISE
    if (authtype_upper == APPLE80211_AUTHTYPE_WPA3_ENTERPRISE || authtype_upper == APPLE80211_AUTHTYPE_WPA3_FT_ENTERPRISE) {
        wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        authtype_upper |= APPLE80211_AUTHTYPE_WPA2;// hack
    }
    
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_SHA256_8021X)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
    }
    
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_akms |= IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_SHA256_PSK;
        wpa.i_enabled = 1;
        memcpy(ic->ic_psk, key, sizeof(ic->ic_psk));
        ic->ic_flags |= IEEE80211_F_PSK;
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_SHA256_8021X)) {
        XYLog("%s %d\n", __FUNCTION__, __LINE__);
        wpa.i_akms |= IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_8021X;	
        wpa.i_enabled = 1;
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    
    if (authtype_lower == APPLE80211_AUTHTYPE_SHARED) {
        XYLog("shared key authentication is not supported!\n");
        return;
    }
    
    if (authtype_upper == APPLE80211_AUTHTYPE_NONE && authtype_lower == APPLE80211_AUTHTYPE_OPEN) { // Open or WEP Open System
        if (key_len > 0) {
            XYLog("%s %d\n", __FUNCTION__, __LINE__);
            nwkey.i_wepon = IEEE80211_NWKEY_WEP;
            nwkey.i_defkid = key_index + 1;
            nwkey.i_key[key_index].i_keylen = (int)key_len;
            nwkey.i_key[key_index].i_keydat = key;
            ieee80211_ioctl_setnwkeys(ic, &nwkey);
        }
    }
}

void AirportItlwm::setPTK(const u_int8_t *key, size_t key_len) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node	* ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    ni->ni_rsn_supp_state = RNSA_SUPP_PTKDONE;
    
    if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP) {
        u_int64_t prsc;
        
        /* check that key length matches that of pairwise cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsncipher);
        if (key_len != keylen) {
            XYLog("PTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        prsc = /*(gtk == NULL) ? LE_READ_6(key->rsc) :*/ 0;
        
        /* map PTK to 802.11 key */
        k = &ni->ni_pairwise_key;
        memset(k, 0, sizeof(*k));
        k->k_cipher = ni->ni_rsncipher;
        k->k_rsc[0] = prsc;
        k->k_len = keylen;
        memcpy(k->k_key, key, k->k_len);
        /* install the PTK */
        if ((*ic->ic_set_key)(ic, ni, k) != 0) {
            XYLog("setting PTK failed\n");
            return;
        }
        else
            XYLog("setting PTK successfully\n");
        ni->ni_flags &= ~IEEE80211_NODE_RSN_NEW_PTK;
        ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
        ni->ni_flags |= IEEE80211_NODE_RXPROT;
    } else if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
        XYLog("%s: unexpected pairwise key update received from %s\n",
              ic->ic_if.if_xname, ether_sprintf(ni->ni_macaddr));
}

void AirportItlwm::setGTK(const u_int8_t *gtk, size_t key_len, u_int8_t kid, u_int8_t *rsc) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node	* ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    if (gtk != NULL) {
        /* check that key length matches that of group cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsngroupcipher);
        if (key_len != keylen) {
            XYLog("GTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        /* map GTK to 802.11 key */
        k = &ic->ic_nw_keys[kid];
        if (k->k_cipher == IEEE80211_CIPHER_NONE || k->k_len != keylen || memcmp(k->k_key, gtk, keylen) != 0) {
            memset(k, 0, sizeof(*k));
            k->k_id = kid;    /* 0-3 */
            k->k_cipher = ni->ni_rsngroupcipher;
            k->k_flags = IEEE80211_KEY_GROUP;
            //if (gtk[6] & (1 << 2))
            //  k->k_flags |= IEEE80211_KEY_TX;
            k->k_rsc[0] = LE_READ_6(rsc);
            k->k_len = keylen;
            memcpy(k->k_key, gtk, k->k_len);
            /* install the GTK */
            if ((*ic->ic_set_key)(ic, ni, k) != 0) {
                XYLog("setting GTK failed\n");
                return;
            }
            else
                XYLog("setting GTK successfully\n");
        }
    }
    
    if (true) {
        ni->ni_flags |= IEEE80211_NODE_TXRXPROT;
#ifndef IEEE80211_STA_ONLY
        if (ic->ic_opmode != IEEE80211_M_IBSS ||
            ++ni->ni_key_count == 2)
#endif
        {
            XYLog("marking port %s valid\n",
                  ether_sprintf(ni->ni_macaddr));
            ieee80211_authorize_port(ic, ni);
            ni->ni_assoc_fail = 0;
            if (ic->ic_opmode == IEEE80211_M_STA)
                ic->ic_rsngroupcipher = ni->ni_rsngroupcipher;
        }
    }
}


bool AirportItlwm::
createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;
    if (primary)
        *primary = NULL;
    
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(0x80, 11000000);
    if (!medium || !IONetworkMedium::addMedium(mediumDict, medium)) {
        if (medium) medium->release();
        mediumDict->release();
        return false;
    }
    medium->release();
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result)
        XYLog("Cannot publish medium dictionary!\n");
    if (result && primary)
        *primary = medium;

    mediumDict->release();
    return result;
}

bool AirportItlwm::start(IOService *provider)
{
    if (!fControllerTimerLock) return false;
    IOLockLock(fControllerTimerLock);
    if (fControllerTimerClosing || fLegacyStartInProgress || fLegacyBaseStarted) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fLegacyStartInProgress = fLegacyBaseStartInProgress = true;
    IOLockUnlock(fControllerTimerLock);
    struct StartOwner {
        AirportItlwm *owner;
        ~StartOwner() { owner->release(); }
    } startOwner{this};
    retain();
    auto failStart = [this, provider]() -> bool {
        IOLockLock(fControllerTimerLock);
        fLegacyStartInProgress = false;
        IOLockUnlock(fControllerTimerLock);
        cleanupLegacy(provider);
        return false;
    };
    int boot_value = 0;
    const bool baseStarted = super::start(provider);
    IOLockLock(fControllerTimerLock);
    fLegacyBaseStartInProgress = false;
    fLegacyBaseStarted = baseStarted;
    IOLockUnlock(fControllerTimerLock);
    if (!baseStarted || controllerTimersClosing()) return failStart();
    OSDictionary *smcMatching = serviceMatching("AppleSMC");
    if (!smcMatching) {
        XYLog("No matching AppleSMC dictionary, failing\n");
        return failStart();
    }
    smcMatching->release();
    pciNub = OSDynamicCast(IOPCIDevice, provider);
    if (!pciNub) {
        return failStart();
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        return failStart();
    }
    if (initPCIPowerManagment(pciNub) == false) {
        return failStart();
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        return failStart();
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        return failStart();
    }
    if (_fWorkloop->addEventSource(_fCommandGate) != kIOReturnSuccess) return failStart();
    fLegacyCommandGateAdded = true;
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        return failStart();
    }
    if (!fHalService->initWithController(this, _fWorkloop, _fCommandGate)) {
        return failStart();
    }
    fHalService->get80211Controller()->ic_event_handler = eventHandler;
    
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    airitl_debug_log = PE_parse_boot_argn("-airitldbg", &boot_value, sizeof(boot_value)) ? 1 : 0;
    
    IOLockLock(fControllerTimerLock);
    if (fControllerTimerClosing) { IOLockUnlock(fControllerTimerLock); return failStart(); }
    fLegacyHalAttachAttempted = fLegacyHalAttachInProgress = true;
    IOLockUnlock(fControllerTimerLock);
    const bool halAttached = fHalService->attach(pciNub);
    IOLockLock(fControllerTimerLock);
    fLegacyHalAttached = halAttached;
    fLegacyHalAttachInProgress = false;
    IOLockUnlock(fControllerTimerLock);
    if (!halAttached || controllerTimersClosing()) {
        XYLog("attach fail\n");
        return failStart();
    }
    IOLockLock(fControllerTimerLock);
    if (fControllerTimerClosing) { IOLockUnlock(fControllerTimerLock); return failStart(); }
    fLegacyInterfaceAttachInProgress = true;
    IOLockUnlock(fControllerTimerLock);
    const bool childAttached = attachInterface((IONetworkInterface **)&fNetIf, true);
    IOLockLock(fControllerTimerLock);
    fLegacyInterfaceAttached = childAttached;
    fLegacyInterfaceAttachInProgress = false;
    IOLockUnlock(fControllerTimerLock);
    if (!childAttached || controllerTimersClosing()) {
        XYLog("attach to interface fail\n");
        return failStart();
    }
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        return failStart();
    }
    if (!holdControllerTimerOwner()) return failStart();
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        return failStart();
    }
    if (fWatchdogWorkLoop->addEventSource(watchdogTimer) != kIOReturnSuccess) {
        return failStart();
    }
    fWatchdogTimerAdded = true;
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
    if (!scanSource || _fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        return failStart();
    }
    fScanSourceAdded = true;
    scanSource->enable();
    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    registerService();
    fNetIf->registerService();
    IOLockLock(fControllerTimerLock);
    const bool closed = fControllerTimerClosing;
    fLegacyStartInProgress = false;
    IOLockUnlock(fControllerTimerLock);
    if (closed) return failStart();
    return true;
}

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    ControllerTimerFrame frame;
    if (!beginControllerTimer(&frame, timer, false)) return;
    if (fHalService) {
        auto *ic = fHalService->get80211Controller();
        if (ic && ic->ic_ac.ac_if.if_watchdog)
            (*ic->ic_ac.ac_if.if_watchdog)(&ic->ic_ac.ac_if);
    }
}

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
    auto *that = OSDynamicCast(AirportItlwm, owner);
    if (!that) return;
    ControllerTimerFrame frame;
    if (!that->beginControllerTimer(&frame, sender, true)) return;
    if (that->getNetworkInterface())
        that->getNetworkInterface()->postMessage(APPLE80211_M_SCAN_DONE);
}

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

bool AirportItlwm::createWorkLoop()
{
    _fWorkloop = IO80211WorkLoop::workLoop();
    return _fWorkloop != 0;
}

IOWorkLoop *AirportItlwm::getWorkLoop() const
{
    return _fWorkloop;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium)
{
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

void AirportItlwm::stop(IOService *provider)
{
    cleanupLegacy(provider);
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            fNetIf->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            fNetIf->stopOutputThread();
            fNetIf->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    IOReturn ret = that->getNetworkInterface()->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    if (that->fAWDLInterface) {
#if __IO80211_TARGET >= __MAC_13_0
        that->fAWDLInterface->setEnabledBySystem(true);
#endif
        that->fAWDLInterface->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    }
    return ret;
}

bool AirportItlwm::cleanupLegacy(IOService *provider, bool freeing)
{
    struct Lifetime {
        AirportItlwm *owner;
        bool held;
        ~Lifetime() { if (held) owner->release(); }
    } lifetime{this, !freeing};
    if (!freeing) retain();
    closeControllerTimerAdmission();
    if (fControllerTimerLock) IOLockLock(fControllerTimerLock);
    if (fLegacyCleanupInProgress) {
        if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fLegacyCleanupInProgress = true;
    if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
    struct CleanupOwner {
        AirportItlwm *owner;
        ~CleanupOwner() {
            if (owner->fControllerTimerLock) IOLockLock(owner->fControllerTimerLock);
            owner->fLegacyCleanupInProgress = false;
            if (owner->fControllerTimerLock) IOLockUnlock(owner->fControllerTimerLock);
        }
    } cleanupOwner{this};
    if (fControllerTimerLock) IOLockLock(fControllerTimerLock);
    const bool starting = fLegacyStartInProgress || fLegacyBaseStartInProgress ||
        fLegacyHalAttachInProgress || fLegacyInterfaceAttachInProgress;
    if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
    if (starting) return false;
    const bool pmQuiesced = unregistPM();
    const bool timersQuiesced = quiesceControllerTimers();
    if (!pmQuiesced || !timersQuiesced) return false;
    if (_fWorkloop && _fWorkloop->inGate()) return false;
    if (fHalService && !fHalService->quiesceForDetach()) return false;
    if (fHalService && fHalService->teardownMustRetainResources()) return false;
    auto *ic = fHalService ? fHalService->get80211Controller() : NULL;
    if (fNetIf) {
        auto *child = fNetIf;
        child->retain();
        auto *childProvider = child->getProvider();
        if (childProvider && childProvider != this) { child->release(); return false; }
        if (!childProvider && !fLegacyInterfaceAttached && !fLegacyChildTerminationRequested) {
            child->release();
            return false;
        }
        if (childProvider == this && !fLegacyChildTerminationRequested) {
            fLegacyInterfaceAttached = true;
            fLegacyChildTerminationRequested = true;
            detachInterface(child, true);
        }
        const bool stopped = child->getProvider() == NULL;
        child->release();
        // Native stop and provider close precede actual detach. A sync request
        // may enqueue or time out; preserve dependencies for external retry.
        if (!stopped) return false;
        fLegacyInterfaceAttached = false;
    } else if (fLegacyInterfaceAttached) return false;
    if (fLegacyBaseStarted) {
        fLegacyBaseStarted = false;
        super::stop(provider);
    }
    fNextNodeToSend = NULL;
    if (fLegacyHalAttachAttempted && fHalService) {
        fHalService->detach(pciNub);
        if (fHalService->teardownMustRetainResources()) return false;
        fLegacyHalAttachAttempted = fLegacyHalAttached = false;
    }
    if (fLegacyEthernetAttached && ic) {
        ether_ifdetach(&ic->ic_ac.ac_if);
        fLegacyEthernetAttached = false;
    }
    if (ic && ic->ic_ac.ac_if.netStat == fpNetStats) ic->ic_ac.ac_if.netStat = NULL;
    fpNetStats = NULL;
    OSSafeReleaseNULL(fNetIf);
    if (_fCommandGate) {
        if (fLegacyCommandGateAdded && (!_fWorkloop ||
            _fWorkloop->removeEventSource(_fCommandGate) != kIOReturnSuccess)) return false;
        fLegacyCommandGateAdded = false;
        OSSafeReleaseNULL(_fCommandGate);
    }
    OSSafeReleaseNULL(fHalService);
    OSSafeReleaseNULL(fWatchdogWorkLoop);
    OSSafeReleaseNULL(_fWorkloop);
    return releaseControllerTimerReferences();
}

void AirportItlwm::releaseAll()
{
    cleanupLegacy(getProvider());
}

void AirportItlwm::free()
{
    if (!cleanupLegacy(getProvider(), true)) return;
    XYLog("%s\n", __FUNCTION__);
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    if (fPowerCallLock) {
        IOLockFree(fPowerCallLock);
        fPowerCallLock = NULL;
    }
    if (fControllerTimerLock) {
        IOLockFree(fControllerTimerLock);
        fControllerTimerLock = NULL;
    }
    super::free();
}

IOReturn AirportItlwm::enable(IONetworkInterface *netif)
{
    if (controllerTimersClosing()) return kIOReturnNotReady;
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(netif);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IONetworkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif)
{
    if (controllerTimersClosing()) return kIOReturnNotReady;
    fHalService->enable(netif);
    if (!armControllerWatchdog()) return kIOReturnNotReady;
    return kIOReturnSuccess;
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    if (controllerTimersClosing()) return;
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportItlwm::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, addrP->bytes);
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
        fHalService->disable(fNetIf);
        fHalService->enable(fNetIf);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::getHardwareAddressForInterface(
                                               IO80211Interface *netif, IOEthernetAddress *addr)
{
    return getHardwareAddress(addr);
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}
#endif

UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

SInt32 AirportItlwm::monitorModeSetEnabled(
                                    IO80211Interface *interface, bool enabled, UInt32 dlt)
{
    return kIOReturnSuccess;
}

bool AirportItlwm::
useAppleRSNSupplicant(IO80211Interface *interface)
{
#ifdef USE_APPLE_SUPPLICANT
    return true;
#else
    return false;
#endif
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void AirportItlwm::beginPowerCall(PowerCallFrame *frame)
{
    IOLockLock(fPowerCallLock);
    frame->thread = current_thread();
    frame->next = fPowerCallFrames;
    fPowerCallFrames = frame;
    IOLockUnlock(fPowerCallLock);
}

void AirportItlwm::endPowerCall(PowerCallFrame *frame)
{
    IOLockLock(fPowerCallLock);
    auto **link = &fPowerCallFrames;
    while (*link && *link != frame) link = &(*link)->next;
    if (*link) {
        *link = frame->next;
        --fPowerCallOutstanding;
    }
    IOLockUnlock(fPowerCallLock);
}

bool AirportItlwm::unregistPM(bool permanent)
{
    if (!fPowerCallLock) return true;
    IOLockLock(fPowerCallLock);
    fPowerCallAdmission = false;
    fPowerCallStopping |= permanent;
    unsigned selfFrames = 0;
    for (auto *frame = fPowerCallFrames; frame; frame = frame->next)
        selfFrames += frame->thread == current_thread();
    if (fPowerCallDraining) {
        IOLockUnlock(fPowerCallLock);
        return false;
    }
    fPowerCallDraining = true;
    thread_call_t calls[2] = {powerOffThreadCall, powerOnThreadCall};
    powerOffThreadCall = powerOnThreadCall = NULL;
    IOLockUnlock(fPowerCallLock);
    for (auto call : calls) {
        if (call && thread_call_cancel(call)) {
            IOLockLock(fPowerCallLock);
            --fPowerCallOutstanding;
            IOLockUnlock(fPowerCallLock);
            release();
        }
    }
    auto yieldGate = [this](void *event) {
        IOCommandGate *gate = getCommandGate();
        if (gate && gate->getWorkLoop() && gate->getWorkLoop()->inGate()) {
            AbsoluteTime deadline;
            clock_interval_to_deadline(10, kMillisecondScale,
                reinterpret_cast<uint64_t *>(&deadline));
            gate->commandSleep(event, deadline, THREAD_UNINT);
        } else IOSleep(10);
    };
    if (selfFrames) {
        // Plain thread calls overlap. Drain all other admitted driver bodies,
        // including workers that have not yet registered their frame.
        for (;;) {
            IOLockLock(fPowerCallLock);
            const bool others = fPowerCallOutstanding > selfFrames;
            IOLockUnlock(fPowerCallLock);
            if (!others) break;
            yieldGate(&fPowerCallOutstanding);
        }
        IOLockLock(fPowerCallLock);
        powerOffThreadCall = calls[0];
        powerOnThreadCall = calls[1];
        fPowerCallDraining = false;
        IOLockUnlock(fPowerCallLock);
        // Our native invocation has not returned. Keep handles, registration
        // retain and resources for a later external drain and cleanup retry.
        return false;
    }
    auto drain = [&](thread_call_t call) -> bool {
        if (!call) return true;
        // isactive counts submitted versus completed, including invocations
        // before frame entry and after body completion until native return.
        while (thread_call_isactive(call)) yieldGate(call);
        return thread_call_free(call);
    };
    if (drain(calls[0])) calls[0] = NULL;
    if (drain(calls[1])) calls[1] = NULL;
    IOLockLock(fPowerCallLock);
    powerOffThreadCall = calls[0];
    powerOnThreadCall = calls[1];
    const bool drained = !calls[0] && !calls[1];
    fPowerCallDraining = false;
    const bool held = drained && fPowerCallOwnerHeld;
    if (drained) {
        pmPolicyMaker = NULL;
        fPowerCallOwnerHeld = false;
    }
    IOLockUnlock(fPowerCallLock);
    if (held) release();
    return drained;
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
    if (!fPowerCallLock) return result;
    IOLockLock(fPowerCallLock);
    if (!fPowerCallAdmission || fPowerCallStopping || pmPowerState == powerStateOrdinal) {
        IOLockUnlock(fPowerCallLock);
        return result;
    }
    thread_call_t call = powerStateOrdinal == kPowerStateOff ? powerOffThreadCall :
        powerStateOrdinal == kPowerStateOn ? powerOnThreadCall : NULL;
    if (call) {
        retain();
        if (thread_call_enter(call)) release();
        else ++fPowerCallOutstanding;
        result = 5000000;
    }
    IOLockUnlock(fPowerCallLock);
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static IOReturn handleSetPowerStateOffGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwm, owner);
    if (!self) return kIOReturnBadArgument;
    IOLockLock(self->fPowerCallLock);
    const bool admitted = self->fPowerCallAdmission && !self->fPowerCallStopping;
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) self->setPowerStateOff();
    else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
    return kIOReturnSuccess;
}

static IOReturn handleSetPowerStateOnGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwm, owner);
    if (!self) return kIOReturnBadArgument;
    IOLockLock(self->fPowerCallLock);
    const bool admitted = self->fPowerCallAdmission && !self->fPowerCallStopping;
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) self->setPowerStateOn();
    else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<AirportItlwm *>(param0);
    AirportItlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    if (gate) gate->runAction(handleSetPowerStateOffGated);
    self->endPowerCall(&frame);
    self->release();
}

static void handleSetPowerStateOn(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<AirportItlwm *>(param0);
    AirportItlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    if (gate) gate->runAction(handleSetPowerStateOnGated);
    self->endPowerCall(&frame);
    self->release();
}

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    if (!policyMaker) return kIOReturnBadArgument;
    if (!fPowerCallLock) return kIOReturnNoMemory;
    IOLockLock(fPowerCallLock);
    const bool busy = fPowerCallStopping || fPowerCallDraining ||
        powerOffThreadCall || powerOnThreadCall || fPowerCallOwnerHeld;
    if (busy) {
        IOLockUnlock(fPowerCallLock);
        return kIOReturnNotReady;
    }
    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;
    powerOffThreadCall = thread_call_allocate(handleSetPowerStateOff, this);
    powerOnThreadCall = thread_call_allocate(handleSetPowerStateOn, this);
    if (!powerOffThreadCall || !powerOnThreadCall) {
        IOLockUnlock(fPowerCallLock);
        unregistPM(false);
        return kIOReturnNoMemory;
    }
    // Keep the recipient alive through the external drain and registration.
    retain();
    fPowerCallOwnerHeld = fPowerCallAdmission = true;
    retain();
    IOLockUnlock(fPowerCallLock);
    IOReturn result = policyMaker->registerPowerDriver(this, powerStateArray, kPowerStateCount);
    IOLockLock(fPowerCallLock);
    const bool stopped = fPowerCallStopping;
    IOLockUnlock(fPowerCallLock);
    if (result != kIOReturnSuccess || stopped) unregistPM(false);
    if (stopped) result = kIOReturnAborted;
    release();
    return result;
}

void AirportItlwm::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
    IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOff;
    IOLockUnlock(fPowerCallLock);
    disableAdapter(fNetIf);
    if (pmPolicyMaker) pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportItlwm::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
    IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOn;
    IOLockUnlock(fPowerCallLock);
    if (pmPolicyMaker) pmPolicyMaker->acknowledgeSetPowerState();
}

int AirportItlwm::
outputRaw80211Packet(IO80211Interface *interface, mbuf_t m)
{
    XYLog("%s len=%zu\n", __FUNCTION__, mbuf_len(m));
    freePacket(m);
    return kIOReturnOutputDropped;
}

UInt32 AirportItlwm::
hardwareOutputQueueDepth(IO80211Interface *interface)
{
    return 0;
}

SInt32 AirportItlwm::
performCountryCodeOperation(IO80211Interface *interface, IO80211CountryCodeOp op)
{
    return 0;
}

SInt32 AirportItlwm::
stopDMA()
{
    if (fNetIf)
        disable(fNetIf);
    return 0;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

int AirportItlwm::
outputActionFrame(OSObject *object, mbuf_t m)
{
    XYLog("%s len=%zu\n", __FUNCTION__, mbuf_len(m));
    mbuf_freem(m);
    return 0;
}

int AirportItlwm::
bpfOutput80211Radio(OSObject *object, mbuf_t m)
{
    XYLog("%s len=%zu\n", __FUNCTION__, mbuf_len(m));
    mbuf_freem(m);
    return 0;
}

SInt32 AirportItlwm::
enableVirtualInterface(IO80211VirtualInterface *interface)
{
    XYLog("%s interface=%s role=%d\n", __FUNCTION__, interface->getBSDName(), interface->getInterfaceRole());
    SInt32 ret = super::enableVirtualInterface(interface);
    if (!ret) {
#if __IO80211_TARGET >= __MAC_13_0
        interface->setEnabledBySystem(true);
#endif
        interface->setLinkState(kIO80211NetworkLinkUp, 0);
        interface->postMessage(APPLE80211_M_LINK_CHANGED);
        return kIOReturnSuccess;
    }
    return ret;
}

SInt32 AirportItlwm::
disableVirtualInterface(IO80211VirtualInterface *interface)
{
    XYLog("%s interface=%s role=%d\n", __FUNCTION__, interface->getBSDName(), interface->getInterfaceRole());
    SInt32 ret = super::disableVirtualInterface(interface);
    if (!ret) {
        interface->setLinkState(kIO80211NetworkLinkDown, 0);
        interface->postMessage(APPLE80211_M_LINK_CHANGED);
        return kIOReturnSuccess;
    }
    return ret;
}

IO80211VirtualInterface *AirportItlwm::
createVirtualInterface(ether_addr *ether, UInt role)
{
    if (role - 1 > 3)
        return super::createVirtualInterface(ether, role);
    IO80211VirtualInterface *inf = new IO80211VirtualInterface;
    if (inf) {
        if (inf->init(this, ether, role, role == APPLE80211_VIF_AWDL ? "awdl" : "p2p"))
            XYLog("%s role=%d succeed\n", __FUNCTION__, role);
        else {
            inf->release();
            return NULL;
        }
    }
    return inf;
}

int AirportItlwm::
bpfOutputPacket(OSObject *object, UInt dltType, mbuf_t m)
{
    XYLog("%s dltType=%d\n", __FUNCTION__, dltType);
    if (dltType == DLT_IEEE802_11_RADIO || dltType == DLT_IEEE802_11)
        return bpfOutput80211Radio(object, m);
    if (dltType == DLT_RAW)
        return outputActionFrame(object, m);
    mbuf_freem(m);
    return 1;
}

void AirportItlwm::
requestPacketTx(void *object, UInt )
{
    UInt32 ret;
    struct TxPacketRequest request;
    if (object == NULL)
        return;
    IO80211VirtualInterface *interface = OSDynamicCast(IO80211VirtualInterface, (OSObject *)object);
    if (interface) {
        memset(&request, 0, sizeof(request));
        if (interface->getInterfaceRole() == APPLE80211_VIF_AWDL) {
//            interface->dequeueTxPackets(&request);
//
//            ret = outputPacket(NULL, interface);
//            if (ret == kIOReturnSuccess) {
//                interface->reportTransmitStatus(NULL, ret, NULL);
//            }
        }
    }
}
