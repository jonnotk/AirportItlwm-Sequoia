//
//  AirportItlwmSkywalkInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//
#include "AirportItlwmV2.hpp"
#include "AirportItlwmSkywalkInterface.hpp"
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
#include "IOPCIEDeviceWrapper.hpp"
#endif
#include <sys/CTimeout.hpp>
#include <libkern/c++/OSMetaClass.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <Airport/AirportItlwmSkywalkPacketPool.h>
#include <Airport/IOSkywalkPacket.h>
#include <Airport/IOSkywalkNetworkPacket.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <HAL/AiritlDebug.h>

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(AirportItlwmSkywalkInterface, IO80211InfraProtocol);

#ifdef AIRITL_DP_DMA
// ---- DMA-capable Skywalk packet pool ---------------------------------------
// Subclass of IOSkywalkPacketBufferPool that attaches an IODMACommand (built from the iwx device's
// IOMapper) to every memory segment it creates, so the framework's nexus-connect can DMA-map the
// pool buffers into the NIC's IOVM space. Without this, base-pool segments have no DMACommand and
// IOSkywalkMemorySegment::prepare() skips device DMA -> the ~4s post-desktop connect fault.
OSDefineMetaClassAndStructors(AirportItlwmSkywalkPacketPool, IOSkywalkPacketBufferPool);

AirportItlwmSkywalkPacketPool *
AirportItlwmSkywalkPacketPool::withMapperName(char const *name, OSObject *owner, uint n,
                                              IOSkywalkPacketBufferPool::PoolOptions const *opts,
                                              IOMapper *mapper)
{
    AirportItlwmSkywalkPacketPool *p = OSTypeAlloc(AirportItlwmSkywalkPacketPool);
    if (!p)
        return NULL;
    // Set the mapper BEFORE init so it is present when segments are created (lazily, at connect).
    p->fMapper = mapper;
    if (mapper)
        mapper->retain();
    if (!p->initWithName(name, owner, n, opts)) {
        p->release();
        return NULL;
    }
    return p;
}

void AirportItlwmSkywalkPacketPool::free(void)
{
    if (fMapper) {
        fMapper->release();
        fMapper = NULL;
    }
    IOSkywalkPacketBufferPool::free();
}

IOReturn AirportItlwmSkywalkPacketPool::
newMemorySegment(IOSkywalkMemorySegmentDescriptor *desc, IOSkywalkMemorySegment **out)
{
    // 1) Let the base create the plain segment (IOSkywalkMemorySegment::withPool).
    IOReturn ret = IOSkywalkPacketBufferPool::newMemorySegment(desc, out);
    if (ret != kIOReturnSuccess || !out || !*out || !fMapper)
        return ret;

    // 2) Build a device DMA command from our mapper and attach it (consumed by segment->prepare()).
    //    Spec verified arg-for-arg against AppleBCMWLANPCIeSkywalkMemorySegment::setMapper:
    //    withSpecification(OutputLittle64, 64, 0, kMapped(0), 0, 0x1000, mapper, NULL). alignment=0x1000
    //    is CORRECT (matches Apple) — do NOT change it.
    IODMACommand *dma = IODMACommand::withSpecification(kIODMACommandOutputLittle64,
                                                        64,                       // numAddressBits
                                                        0,                        // maxSegmentSize (unlimited)
                                                        IODMACommand::kMapped,    // == 0, matches Apple
                                                        0,                        // maxTransferSize
                                                        0x1000,                   // alignment (page) — verified
                                                        fMapper,                  // device IOMapper
                                                        NULL);                    // refCon
    if (!dma)
        return ret;   // segment exists but without DMA; prepare() will skip DMA (no worse than base)
    // setDMACommand stores the command on the segment (seg+0x38). We deliberately do NOT release our
    // +1 ref here: the segment becomes the owner (it is not proven to retain), avoiding any UAF; the
    // command lives for the pool's lifetime.
    (*out)->setDMACommand(dma, 0);
    return ret;
}
#endif /* AIRITL_DP_DMA */

// Datapath sizing (tiny — baseline, no real traffic).
#define AIRITL_DP_QUEUE_CAPACITY 256
#define AIRITL_DP_BUFFER_SIZE    2048
#define AIRITL_DP_MAX_BUFFERS_PER_PACKET 1
static_assert(AIRITL_DP_QUEUE_CAPACITY >= 8, "TX submission requires at least eight packets");
static_assert(AIRITL_DP_MAX_BUFFERS_PER_PACKET == 1,
              "The copied-packet transport and pool flags require one buffer per packet");

const char* hexdump(uint8_t *buf, size_t len) {
    ssize_t str_len = len * 3 + 1;
    char *str = (char*)IOMalloc(str_len);
    if (!str)
        return nullptr;
    for (size_t i = 0; i < len; i++)
    snprintf(str + 3 * i, (len - i) * 3, "%02x ", buf[i]);
    str[MAX(str_len - 2, 0)] = 0;
    return str;
}

static int ieeeChanFlag2appleScanFlagVentura(int flags)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    ret |= (APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ);
    return ret;
}

static int ieeeChanFlag2apple(int flags, int bw)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    if (!(flags & IEEE80211_CHAN_PASSIVE))
        ret |= APPLE80211_C_FLAG_ACTIVE;
    if (flags & IEEE80211_CHAN_DFS)
        ret |= APPLE80211_C_FLAG_DFS;
    if (bw == -1) {
        if (flags & IEEE80211_CHAN_VHT) {
            if ((flags & IEEE80211_CHAN_VHT160) || (flags & IEEE80211_CHAN_VHT80_80))
                ret |= APPLE80211_C_FLAG_160MHZ;
            if (flags & IEEE80211_CHAN_VHT80)
                ret |= APPLE80211_C_FLAG_80MHZ;
        } else if ((flags & IEEE80211_CHAN_HT40) && (flags & IEEE80211_CHAN_HT)) {
            ret |= APPLE80211_C_FLAG_40MHZ;
            if (flags & IEEE80211_CHAN_HT40U)
                ret |= APPLE80211_C_FLAG_EXT_ABV;
        } else if (flags & IEEE80211_CHAN_HT20)
            ret |= APPLE80211_C_FLAG_20MHZ;
        else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
            ret |= APPLE80211_C_FLAG_10MHZ;
    } else {
        switch (bw) {
            case IEEE80211_CHAN_WIDTH_80P80:
            case IEEE80211_CHAN_WIDTH_160:
                ret |= APPLE80211_C_FLAG_160MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_80:
                ret |= APPLE80211_C_FLAG_80MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_40:
                ret |= APPLE80211_C_FLAG_40MHZ;
                if (flags & IEEE80211_CHAN_HT40U)
                    ret |= APPLE80211_C_FLAG_EXT_ABV;
                break;
            case IEEE80211_CHAN_WIDTH_20:
                ret |= APPLE80211_C_FLAG_20MHZ;
                break;
            default:
                if (flags & IEEE80211_CHAN_HT20)
                    ret |= APPLE80211_C_FLAG_20MHZ;
                else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
                    ret |= APPLE80211_C_FLAG_10MHZ;
                break;
        }
    }
    return ret;
}

#if __IO80211_TARGET >= __MAC_15_0
void AirportItlwmSkywalkInterface::setEnabledBySystem(bool)
{
}

bool AirportItlwmSkywalkInterface::enabledBySystem(void)
{
    return true;
}

bool AirportItlwmSkywalkInterface::willRoam(ether_addr *, UInt)
{
    return false;
}

void AirportItlwmSkywalkInterface::setPoweredOnByUser(bool)
{
}

void AirportItlwmSkywalkInterface::setCurrentBssid(ether_addr *)
{
}
#endif

void AirportItlwmSkywalkInterface::setSelfMacAddr(ether_addr *addr)
{
    if (!addr || !fHalService)
        return;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return;

    if_setlladdr(&ic->ic_ac.ac_if, addr->octet);
}

#if __IO80211_TARGET >= __MAC_15_0
void *AirportItlwmSkywalkInterface::getCurrentBssid(void)
{
    if (!fHalService)
        return NULL;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss)
        return NULL;

    return ic->ic_bss->ni_bssid;
}
#endif

IOReturn AirportItlwmSkywalkInterface::associateSSID(uint8_t *ssid, uint32_t ssid_len, const struct ether_addr &bssid, uint32_t authtype_lower, uint32_t authtype_upper, uint8_t *key, uint32_t key_len, int key_index)
{
    if ((ssid_len > 0 && !ssid) || ssid_len > IEEE80211_NWID_LEN)
        return kIOReturnBadArgument;
    if (!fHalService)
        return kIOReturnNotAttached;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnNotAttached;

    struct ieee80211_wpaparams     wpa;
    struct ieee80211_nwkey         nwkey;
    bzero(&wpa, sizeof(wpa));
    bzero(&nwkey, sizeof(nwkey));

    if (authtype_lower == APPLE80211_AUTHTYPE_SHARED) {
        XYLog("shared key authentication is not supported!\n");
        return kIOReturnUnsupported;
    }

    if (authtype_upper & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK)) {
        if (!key || key_len < sizeof(ic->ic_psk)) {
            XYLog("missing or short PSK material. expected at least %zu, got %u\n", sizeof(ic->ic_psk), key_len);
            return kIOReturnBadArgument;
        }
    }

    if (authtype_upper == APPLE80211_AUTHTYPE_NONE && authtype_lower == APPLE80211_AUTHTYPE_OPEN && key_len > 0) {
        if (!key || key_index < 0 || (size_t)key_index >= ARRAY_SIZE(nwkey.i_key) || key_len > APPLE80211_KEY_BUFF_LEN) {
            XYLog("invalid WEP key request. index=%d len=%u\n", key_index, key_len);
            return kIOReturnBadArgument;
        }
    }

    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    
    memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
    if (ssid_len != 0)
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
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setPTK(const u_int8_t *key, size_t key_len) {
    if (!key)
        return kIOReturnBadArgument;
    if (!fHalService)
        return kIOReturnNotAttached;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || !ic->ic_bss || !ic->ic_set_key)
        return kIOReturnNotAttached;

    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
#if __IO80211_TARGET < __MAC_15_0
    ni->ni_rsn_supp_state = RNSA_SUPP_PTKDONE;
#endif
    
    if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP) {
        u_int64_t prsc;
        
        /* check that key length matches that of pairwise cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsncipher);
        if (key_len != keylen) {
            XYLog("PTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return kIOReturnBadArgument;
        }
        prsc = /*(gtk == NULL) ? LE_READ_6(key->rsc) :*/ 0;
        
        /* map PTK to 802.11 key */
        k = &ni->ni_pairwise_key;
        if (keylen <= 0 || (size_t)keylen > sizeof(k->k_key)) {
            XYLog("PTK length out of bounds: %d\n", keylen);
            return kIOReturnBadArgument;
        }
#if __IO80211_TARGET >= __MAC_15_0
        // A repeated installation must retain the packet number and replay state.
        if ((ni->ni_flags & IEEE80211_NODE_RXPROT) && k->k_cipher == ni->ni_rsncipher &&
            k->k_len == keylen && memcmp(k->k_key, key, keylen) == 0)
            return kIOReturnSuccess;
        struct ieee80211_key previous = *k;
#endif
        memset(k, 0, sizeof(*k));
        k->k_cipher = ni->ni_rsncipher;
        k->k_rsc[0] = prsc;
        k->k_len = keylen;
        memcpy(k->k_key, key, k->k_len);
        /* install the PTK */
        if ((*ic->ic_set_key)(ic, ni, k) != 0) {
#if __IO80211_TARGET >= __MAC_15_0
            if (k->k_priv && k->k_priv != previous.k_priv)
                ieee80211_delete_key(ic, ni, k);
            *k = previous;
            bzero(&previous, sizeof(previous));
#endif
            XYLog("setting PTK failed\n");
            return kIOReturnError;
        }
        else
            XYLog("setting PTK successfully\n");
#if __IO80211_TARGET >= __MAC_15_0
        // Keep the live key at its original address. TKIP contexts refer into it.
        if (previous.k_priv)
            ieee80211_delete_key(ic, ni, &previous);
        bzero(&previous, sizeof(previous));
        ni->ni_rsn_supp_state = RNSA_SUPP_PTKDONE;
        ni->ni_port_valid = 0;
#endif
        ni->ni_flags &= ~IEEE80211_NODE_RSN_NEW_PTK;
        ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
        ni->ni_flags |= IEEE80211_NODE_RXPROT;
    } else if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
        XYLog("%s: unexpected pairwise key update received from %s\n",
              ic->ic_if.if_xname, ether_sprintf(ni->ni_macaddr));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setGTK(const u_int8_t *gtk, size_t key_len, u_int8_t kid, u_int8_t *rsc) {
    if (!fHalService)
        return kIOReturnNotAttached;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || !ic->ic_bss || !ic->ic_set_key)
        return kIOReturnNotAttached;

    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
#if __IO80211_TARGET >= __MAC_15_0
    if (!gtk || !rsc || kid >= 4 ||
        (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP &&
         (!ni->ni_pairwise_key.k_len || !(ni->ni_flags & IEEE80211_NODE_RXPROT))))
        return kIOReturnBadArgument;
#endif
    if (gtk != NULL) {
        if (!rsc || kid >= ARRAY_SIZE(ic->ic_nw_keys)) {
            XYLog("invalid GTK request. kid=%u\n", kid);
            return kIOReturnBadArgument;
        }
        /* check that key length matches that of group cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsngroupcipher);
        if (key_len != keylen) {
            XYLog("GTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return kIOReturnBadArgument;
        }
        /* map GTK to 802.11 key */
        k = &ic->ic_nw_keys[kid];
        if (keylen <= 0 || (size_t)keylen > sizeof(k->k_key)) {
            XYLog("GTK length out of bounds: %d\n", keylen);
            return kIOReturnBadArgument;
        }
#if __IO80211_TARGET >= __MAC_15_0
        if (k->k_cipher != ni->ni_rsngroupcipher || k->k_len != keylen || memcmp(k->k_key, gtk, keylen) != 0) {
#else
        if (k->k_cipher == IEEE80211_CIPHER_NONE || k->k_len != keylen || memcmp(k->k_key, gtk, keylen) != 0) {
#endif
#if __IO80211_TARGET >= __MAC_15_0
            struct ieee80211_key previous = *k;
#endif
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
#if __IO80211_TARGET >= __MAC_15_0
                if (k->k_priv && k->k_priv != previous.k_priv)
                    ieee80211_delete_key(ic, ni, k);
                *k = previous;
                bzero(&previous, sizeof(previous));
#endif
                XYLog("setting GTK failed\n");
                return kIOReturnError;
            }
            else
                XYLog("setting GTK successfully\n");
#if __IO80211_TARGET >= __MAC_15_0
            if (previous.k_priv)
                ieee80211_delete_key(ic, ni, &previous);
            bzero(&previous, sizeof(previous));
#endif
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
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::
init(IOService *provider)
{
#if __IO80211_TARGET >= __MAC_15_0
    fSourceInitPhase = SourceInitPhase::InProgress;
    fFailedSourceInitDisposalPrepared = false;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    fFailedNativeStartDisposalPrepared = false;
#endif
#endif
    // Record each native phase separately from complete custom initialization.
    fNativeInitComplete = false;
    fNativeIdentityInitAttempted = false;
    fNativeIdentityInitComplete = false;
    fCustomInitComplete = false;
    fNativeStartPhase = NativeStartPhase::NotAttempted;
    fNativeStopPhase = NativeStopPhase::NotCalled;
    fLocalCleanupState = LocalCleanupState::Idle;
    fLocalCleanupAttempts = 0;
    fCleanupDisableComplete = false;
    fLocalCleanupFailure = kIOReturnSuccess;
    fLocalCleanupReason = nullptr;
    instance = NULL;
    fHalService = NULL;
    scanSource = NULL;
    fScanRequestPending = 0;
#if __IO80211_TARGET >= __MAC_15_0
    fDatapathRetiring = false;
    fNativeScan = NULL; fNativeJoin = NULL;
    fNativeScanState.quiesced(); fNativeJoinState.quiesced();
    fNativeAssocReported = false; fNativeFirstBeaconReported = false;
    fNativeConnectReported = false; fNativeJoinAbortPending = false; fNativeObserverReady = false;
    fNativeResetPending = fNativeResetQuiescencePending = false;
    fNativeLinkReported = false; fNativeJoinDispatching = false;
    fNativeBssValid = false; fNativeBssConfigured = false; fNativeBssGeneration = 0;
    fNativeControlSequence = fNativeControlOwner = 0;
    bzero(&fNativeBss, sizeof(fNativeBss));
    bzero(&fNativeRoam, sizeof(fNativeRoam));
    bzero(fNativeJoinBssid, sizeof(fNativeJoinBssid));
#endif
    fNextNodeToSend = NULL;
    fScanResultWrapping = false;
    current_authtype_lower = 0;
    current_authtype_upper = 0;
    disassocIsVoluntary = false;
    fPacketPool = NULL;
    fTxSubQueue = NULL;
    fTxCompQueue = NULL;
    fRxSubQueue = NULL;
    fRxCompQueue = NULL;
    fMCastQueue = NULL;
    fDatapathProvisioned = false;
    fDatapathEnabled = false;
    fFrameworkStarted = false;
    fRegistrationAttempted = false;
    fDatapathWorkQueue = NULL;
    fBoundQueueMask = 0;
#if __IO80211_TARGET >= __MAC_15_0
    fPacketTransport.initialize(AIRITL_DP_BUFFER_SIZE);
    fStopThread = nullptr;
    fRegistrationSucceeded = false;
    fTransportRetryTimer = nullptr;
    fTransportRetryTimerBound = false;
    fTransportRetryPending = false;
    fTransportRetryRequired = false;
#endif
    fFirstNativeInitPhase = FirstNativeInitPhase::InProgress;
    bool ret = IO80211InfraInterface::init();
    fNativeInitComplete = ret;
    fFirstNativeInitPhase = ret ? FirstNativeInitPhase::Succeeded : FirstNativeInitPhase::Failed;
    if (!ret) {
        XYLog("%s IO80211InfraInterface init failed\n", __PRETTY_FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
        fSourceInitPhase = SourceInitPhase::ReturnedFailed;
#endif
        return false;
    }
    instance = OSDynamicCast(AirportItlwm, provider);
#if __IO80211_TARGET >= __MAC_15_0
    if (!instance) {
        fSourceInitPhase = SourceInitPhase::ReturnedFailed;
        return false;
    }
#else
    if (!instance)
        return false;
#endif
    fHalService = instance->fHalService;
    scanSource = instance->scanSource;
#if __IO80211_TARGET >= __MAC_15_0
    IOEthernetAddress hardwareAddress = {};
    if (instance->getHardwareAddress(&hardwareAddress) != kIOReturnSuccess) {
        fSourceInitPhase = SourceInitPhase::ReturnedFailed;
        return false;
    }
    ether_addr macAddress = {};
    static_assert(sizeof(macAddress.octet) == sizeof(hardwareAddress.bytes), "MAC size mismatch");
    memcpy(macAddress.octet, hardwareAddress.bytes, sizeof(macAddress.octet));
    // Native two-argument init reuses the just-created Skywalk ivars and stores
    // this MAC. It does not dispatch the subclass's zero-argument initializer.
    fNativeIdentityInitAttempted = true;
    fNativeIdentityInitComplete = IO80211SkywalkInterface::init(provider, &macAddress);
    if (!fNativeIdentityInitComplete) {
        fSourceInitPhase = SourceInitPhase::ReturnedFailed;
        return false;
    }
#endif
    fCustomInitComplete = true;
#if __IO80211_TARGET >= __MAC_15_0
    fSourceInitPhase = SourceInitPhase::Succeeded;
#endif
    return true;
}

bool AirportItlwmSkywalkInterface::start(IOService *provider)
{
    if (!controllerFunctionalReady() || fFrameworkStarted ||
        fNativeStartPhase != NativeStartPhase::NotAttempted)
        return false;
    fNativeStartPhase = NativeStartPhase::InProgress;
    fFrameworkStarted = IO80211InfraInterface::start(provider);
    fNativeStartPhase = fFrameworkStarted ? NativeStartPhase::Succeeded : NativeStartPhase::Failed;
    return fFrameworkStarted;
}

void AirportItlwmSkywalkInterface::stop(IOService *provider)
{
    if (!controllerCleanupReady()) return;
#if __IO80211_TARGET >= __MAC_15_0
    // Registration may not have populated fDatapathWorkQueue yet. The native
    // stop exchange sleeps on the controller's actual provider workloop.
    IOService *stopProvider = getProvider();
    if (!stopProvider)
        stopProvider = provider;
    if (!stopProvider) {
        IOLog("AirportItlwmSkywalkInterface::stop missing provider\n");
        return;
    }
    stopProvider->retain();
    IOWorkLoop *workLoop = stopProvider->getWorkLoop();
    if (!workLoop) {
        IOLog("AirportItlwmSkywalkInterface::stop missing provider workloop\n");
        stopProvider->release();
        return;
    }
    workLoop->retain();
    auto controller = OSDynamicCast(AirportItlwm, stopProvider);
    IOCommandGate *gate = controller ? controller->getCommandGate() : nullptr;
    if (!gate || gate->getWorkLoop() != workLoop) {
        IOLog("AirportItlwmSkywalkInterface::stop missing provider command gate\n");
        workLoop->release();
        stopProvider->release();
        return;
    }
    gate->retain();
#ifdef AIRITL_LINK_DEFER
    // A deferred link publication may be queued, waiting for the gate, or
    // yielded inside the native setter. Drain it before native stop.
    if (!controller->quiesceLinkStateCall()) {
        IOLog("AirportItlwmSkywalkInterface::stop deferred link publication not drained\n");
        gate->release();
        workLoop->release();
        stopProvider->release();
        return;
    }
#endif
    // runAction also permits recursive entry from normal gated termination.
    IOReturn result = workLoop->runAction(stopGatedAction, this, stopProvider, gate);
    gate->release();
    workLoop->release();
    stopProvider->release();
    if (result != kIOReturnSuccess)
        IOLog("AirportItlwmSkywalkInterface::stop admission failed 0x%x\n", result);
#else
    if (fLocalCleanupState == LocalCleanupState::Complete) return;
    if (!beginLocalCleanup() || !disableForCleanup()) return;
    if (fFrameworkStarted) {
        fFrameworkStarted = false;
        fNativeStopPhase = NativeStopPhase::InProgress;
        IO80211InfraInterface::stop(provider);
        fNativeStopPhase = NativeStopPhase::Returned;
    }
    if (!finishDatapathCleanup()) return;
    detachControllerReferences();
#endif
}

#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::stopGatedAction(OSObject *owner, void *arg0,
                                                     void *arg1, void *, void *)
{
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    auto provider = static_cast<IOService *>(arg0);
    auto gate = static_cast<IOCommandGate *>(arg1);
    if (!interface || !interface->controllerCleanupReady())
        return kIOReturnNotReady;
    IOWorkLoop *workLoop = provider ? provider->getWorkLoop() : nullptr;
    if (!workLoop || !workLoop->inGate() ||
        !gate || gate->getWorkLoop() != workLoop)
        return kIOReturnNotReady;

    // Native stop may sleep and release A. Another stop must not tear down its
    // dependencies during that exchange, even though native start is cleared.
    while (interface->fStopThread) {
        if (interface->fStopThread == IOThreadSelf())
            return kIOReturnSuccess;
        if (gate->commandSleep(&interface->fStopThread, THREAD_UNINT) != THREAD_AWAKENED)
            return kIOReturnError;
    }
    if (!interface->controllerCleanupReady())
        return kIOReturnNotReady;
    if (interface->fLocalCleanupState == LocalCleanupState::Complete)
        return kIOReturnSuccess;
    if (!interface->beginLocalCleanup()) return kIOReturnNotReady;
    interface->fStopThread = IOThreadSelf();
    if (!interface->disableForCleanup()) {
        interface->fStopThread = nullptr;
        gate->commandWakeup(&interface->fStopThread, false);
        return kIOReturnNotReady;
    }
    if (interface->fFrameworkStarted) {
        interface->fFrameworkStarted = false;
        interface->fNativeStopPhase = NativeStopPhase::InProgress;
        interface->IO80211InfraInterface::stop(provider);
        // This records the call return only. Native callback completion remains
        // a separate obligation of deregistration and the completion query.
        interface->fNativeStopPhase = NativeStopPhase::Returned;
    }
    const bool complete = interface->finishDatapathCleanup();
    if (complete)
        interface->detachControllerReferences();
    interface->fStopThread = nullptr;
    gate->commandWakeup(&interface->fStopThread, false);
    return complete ? kIOReturnSuccess : kIOReturnNotReady;
}
#endif

#if __IO80211_TARGET >= __MAC_15_0
bool AirportItlwmSkywalkInterface::failedSourceInitHasEmptyResources() const
{
    return fSourceInitPhase == SourceInitPhase::ReturnedFailed &&
        fFirstNativeInitPhase == FirstNativeInitPhase::Succeeded && fNativeInitComplete &&
        !fNativeIdentityInitComplete && !fCustomInitComplete &&
        fNativeStartPhase == NativeStartPhase::NotAttempted &&
        fNativeStopPhase == NativeStopPhase::NotCalled && !fFrameworkStarted && !fStopThread &&
        fLocalCleanupState == LocalCleanupState::Idle && !fLocalCleanupAttempts &&
        !fCleanupDisableComplete && fLocalCleanupFailure == kIOReturnSuccess && !fLocalCleanupReason &&
        !fDatapathRetiring && !fDatapathProvisioned && !fDatapathEnabled &&
        !fPacketPool && !fTxSubQueue && !fTxCompQueue && !fRxSubQueue && !fRxCompQueue && !fMCastQueue &&
        !fDatapathWorkQueue && !fBoundQueueMask && !fRegistrationAttempted && !fRegistrationSucceeded &&
        !fTransportRetryTimer && !fTransportRetryTimerBound && !fTransportRetryPending && !fTransportRetryRequired &&
        !fPacketTransport.isEnabled() && !fPacketTransport.txPending() && !fPacketTransport.rxPending() &&
        !fNativeScan && !fNativeJoin && !fNativeScanState.active && !fNativeScanState.draining &&
        !fNativeScanState.drainingGeneration && !fNativeScanState.submitted &&
        !fNativeScanState.cancelling && !fNativeScanState.deadline &&
        !fNativeJoinState.active && !fNativeJoinState.draining && !fNativeJoinState.drainingGeneration &&
        !fNativeRoam.active && !fNativeRoam.draining && !fNativeJoinDispatching && !fNativeControlOwner;
}

bool AirportItlwmSkywalkInterface::prepareFailedSourceInitDisposal(AirportItlwm *controller)
{
    // The start caller owns this exact child before preparation and release.
    if (!controller || fFailedSourceInitDisposalPrepared ||
        (instance && instance != controller) || !failedSourceInitHasEmptyResources() || getProvider())
        return false;
    // Keep the same source facts after the positive-owner provider query.
    if ((instance && instance != controller) || !failedSourceInitHasEmptyResources())
        return false;
    detachControllerReferences();
    fNativeObserverReady = false;
    fNativeJoinAbortPending = false;
    fNativeResetPending = false;
    fNativeResetQuiescencePending = false;
    fNativeAssocReported = false;
    fNativeFirstBeaconReported = false;
    fNativeConnectReported = false;
    fNativeLinkReported = false;
    fNativeBssValid = false;
    fNativeBssConfigured = false;
    fFailedSourceInitDisposalPrepared = true;
    return true;
}

bool AirportItlwmSkywalkInterface::failedSourceInitDisposalReady() const
{
    // Provider absence was checked while the caller still owned the child.
    return fFailedSourceInitDisposalPrepared && failedSourceInitHasEmptyResources() &&
        !instance && !fHalService && !scanSource && !fNextNodeToSend &&
        !fScanResultWrapping && !fScanRequestPending && !fNativeObserverReady &&
        !fNativeJoinAbortPending && !fNativeResetPending && !fNativeResetQuiescencePending &&
        !fNativeAssocReported && !fNativeFirstBeaconReported && !fNativeConnectReported &&
        !fNativeLinkReported && !fNativeBssValid && !fNativeBssConfigured;
}
#endif

bool AirportItlwmSkywalkInterface::controllerCleanupReady() const
{
    // Full custom initialization is required for operation, not for disposal of
    // a first-init-success child that never entered native start.
    return fNativeInitComplete &&
        (!fNativeIdentityInitAttempted || fNativeIdentityInitComplete) &&
        (fNativeStartPhase == NativeStartPhase::NotAttempted ||
         fNativeStartPhase == NativeStartPhase::Succeeded);
}

bool AirportItlwmSkywalkInterface::controllerFunctionalReady() const
{
    return controllerCleanupReady() && fCustomInitComplete &&
        fLocalCleanupState == LocalCleanupState::Idle;
}

bool AirportItlwmSkywalkInterface::datapathTeardownComplete() const
{
    if (fLocalCleanupState != LocalCleanupState::Idle &&
        fLocalCleanupState != LocalCleanupState::Complete) return false;
#if __IO80211_TARGET >= __MAC_15_0
    if (fTransportRetryTimer || fTransportRetryTimerBound || fRegistrationSucceeded ||
        fTransportRetryPending || fTransportRetryRequired) return false;
#endif
    return !fDatapathEnabled && !fDatapathProvisioned &&
        !fRegistrationAttempted && !fBoundQueueMask && !fDatapathWorkQueue &&
        !fPacketPool && !fTxSubQueue && !fTxCompQueue && !fRxSubQueue && !fRxCompQueue && !fMCastQueue;
}

bool AirportItlwmSkywalkInterface::controllerStopComplete() const
{
    if (!controllerCleanupReady() || !datapathTeardownComplete()) return false;
#if __IO80211_TARGET >= __MAC_15_0
    if (fStopThread) return false;
#endif
    if (fNativeStartPhase == NativeStartPhase::Succeeded &&
        fNativeStopPhase != NativeStopPhase::Returned) return false;
    return !fFrameworkStarted && fNativeStopPhase != NativeStopPhase::InProgress &&
        !instance && !fHalService && !scanSource && !fNextNodeToSend;
}

bool AirportItlwmSkywalkInterface::finishUnstartedCleanup()
{
    // Separate empty, never-attached path when no controller gate was acquired.
    if (!controllerCleanupReady() || getProvider() ||
        fNativeStartPhase != NativeStartPhase::NotAttempted || fFrameworkStarted ||
        fNativeStopPhase != NativeStopPhase::NotCalled || !datapathTeardownComplete())
        return false;
#if __IO80211_TARGET >= __MAC_15_0
    if (fStopThread) return false;
#endif
    if (!teardownDatapath()) return false;
    detachControllerReferences();
    return true;
}

bool AirportItlwmSkywalkInterface::finishControllerCleanup(AirportItlwm *controller,
    IOCommandGate *gate, IOWorkLoop *originalLoop, bool neverAttached)
{
    // The controller action proves exact structural ownership and holds both
    // objects before entry. This method never invokes native stop or matching.
    if (!controllerCleanupReady() || !controller || !gate || !originalLoop ||
        gate->getWorkLoop() != originalLoop || !originalLoop->inGate() ||
        (instance && instance != controller) || getProvider() || fFrameworkStarted ||
        fNativeStopPhase == NativeStopPhase::InProgress ||
        fLocalCleanupState == LocalCleanupState::InProgress)
        return false;
#if __IO80211_TARGET >= __MAC_15_0
    if (fStopThread) return false;
#endif
    if (fNativeStartPhase == NativeStartPhase::Succeeded) {
        if (neverAttached || fNativeStopPhase != NativeStopPhase::Returned) return false;
    } else if (!neverAttached && fNativeStartPhase != NativeStartPhase::NotAttempted) {
        return false;
    }
    if (fDatapathWorkQueue && fDatapathWorkQueue != originalLoop)
        return failLocalCleanup("foreign original datapath loop", kIOReturnBadArgument);
    if (!teardownDatapath()) return false;
    detachControllerReferences();
    return true;
}

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)

extern IOCommandGate *_fCommandGate;
extern IO80211WorkQueue *_fWorkloop;
bool AirportItlwmSkywalkInterface::validateFailedNativeStartAbort(AirportItlwm *controller)
{
    if (!controller || instance != controller || controller->fNetIf != this ||
        !fHalService || fHalService != controller->fHalService ||
        !controller->fWrapperProvider || !controller->fCommandGateAdded ||
        !_fCommandGate || !_fWorkloop || _fCommandGate->getWorkLoop() != _fWorkloop ||
        !_fWorkloop->inGate() || controller->fWrapperProvider->getWorkLoop() != _fWorkloop ||
        getProvider() != controller || controller->getProvider() != controller->fWrapperProvider ||
        !controller->fInterfaceAttached || !controller->fControllerInterfaceAttached ||
        controller->fInterfacePublished || controller->fChildTerminationRequested ||
        controller->fEthernetAttached || controller->bsdInterface ||
        controller->fNativeHooksInstalled || controller->fNativeBootstrapDispatched ||
        controller->fNativeResetActive || controller->fNativeAvailabilityDispatched ||
        fFailedNativeStartDisposalPrepared || fSourceInitPhase != SourceInitPhase::Succeeded ||
        fFirstNativeInitPhase != FirstNativeInitPhase::Succeeded || !fNativeInitComplete ||
        !fNativeIdentityInitComplete || !fCustomInitComplete ||
        fNativeStartPhase != NativeStartPhase::Failed || fFrameworkStarted ||
        fNativeStopPhase != NativeStopPhase::NotCalled || fStopThread ||
        fLocalCleanupState != LocalCleanupState::Idle || fLocalCleanupAttempts ||
        fCleanupDisableComplete || fLocalCleanupFailure != kIOReturnSuccess || fLocalCleanupReason ||
        !fDatapathProvisioned || fDatapathEnabled || fDatapathRetiring ||
        !fRxCompQueue || !fRxSubQueue || !fTxCompQueue || !fTxSubQueue || !fMCastQueue || !fPacketPool ||
        fPacketPool->getMetaClass() != IOSkywalkPacketBufferPool::metaClass ||
        fDatapathWorkQueue || fBoundQueueMask || fRegistrationAttempted || fRegistrationSucceeded ||
        fTransportRetryTimer || fTransportRetryTimerBound || fTransportRetryPending || fTransportRetryRequired ||
        fPacketTransport.isEnabled() || fPacketTransport.txPending() || fPacketTransport.rxPending() ||
        fNativeScan || fNativeJoin || fNativeScanState.active || fNativeScanState.draining ||
        fNativeScanState.drainingGeneration || fNativeScanState.submitted || fNativeScanState.cancelling ||
        fNativeScanState.deadline || fNativeJoinState.active || fNativeJoinState.draining ||
        fNativeJoinState.drainingGeneration || fNativeRoam.active || fNativeRoam.draining ||
        fNativeJoinDispatching || fNativeControlOwner || fNativeControlSequence || fNativeObserverReady ||
        fNativeScanState.generation != 1 || fNativeJoinState.generation != 1 || fNativeRoam.generation ||
        fNativeBssGeneration || fNativeAssocReported || fNativeFirstBeaconReported || fNativeConnectReported ||
        fNativeLinkReported || fNativeBssValid || fNativeBssConfigured ||
        fNativeResetPending || fNativeResetQuiescencePending || fNativeJoinAbortPending ||
        fNextNodeToSend || fScanRequestPending || fScanResultWrapping ||
        scanSource != (controller->scanSource ? controller->scanSource : controller->fRetiredScanTimer))
        return false;
    auto *ic = fHalService->get80211Controller();
    return ic && ic->ic_ac.ac_if.controller == controller && !ic->ic_native_controlled &&
        ic->ic_opmode == IEEE80211_M_STA && ic->ic_state == IEEE80211_S_INIT &&
        !(ic->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING));
}
bool AirportItlwmSkywalkInterface::takeFailedNativeStartOwners(FailedNativeStartOwners *owners)
{
    if (!owners || owners->rxCompletion || owners->rxSubmission || owners->txCompletion ||
        owners->txSubmission || owners->multicast || owners->pool ||
        !validateFailedNativeStartAbort(instance))
        return false;
    fLocalCleanupState = LocalCleanupState::InProgress;
    owners->rxCompletion = fRxCompQueue; owners->rxSubmission = fRxSubQueue;
    owners->txCompletion = fTxCompQueue; owners->txSubmission = fTxSubQueue;
    owners->multicast = fMCastQueue; owners->pool = fPacketPool;
    fRxCompQueue = NULL; fRxSubQueue = NULL; fTxCompQueue = NULL;
    fTxSubQueue = NULL; fMCastQueue = NULL; fPacketPool = NULL;
    fDatapathProvisioned = false;
    return true;
}
bool AirportItlwmSkywalkInterface::finishFailedNativeStartDetach(AirportItlwm *controller)
{
    if (!controller || instance != controller || controller->fNetIf != this ||
        getProvider() || controller->fInterfaceAttached || controller->fControllerInterfaceAttached ||
        controller->fInterfacePublished || fFailedNativeStartDisposalPrepared ||
        fSourceInitPhase != SourceInitPhase::Succeeded ||
        fFirstNativeInitPhase != FirstNativeInitPhase::Succeeded || !fNativeInitComplete ||
        !fNativeIdentityInitComplete || !fCustomInitComplete ||
        fNativeStartPhase != NativeStartPhase::Failed || fNativeStopPhase != NativeStopPhase::NotCalled ||
        fFrameworkStarted || fStopThread || fLocalCleanupState != LocalCleanupState::InProgress ||
        fLocalCleanupAttempts || fCleanupDisableComplete || fLocalCleanupFailure != kIOReturnSuccess || fLocalCleanupReason ||
        fPacketPool || fRxCompQueue || fRxSubQueue || fTxCompQueue || fTxSubQueue || fMCastQueue ||
        fDatapathProvisioned || fDatapathEnabled || fDatapathRetiring || fDatapathWorkQueue ||
        fBoundQueueMask || fRegistrationAttempted || fRegistrationSucceeded ||
        fTransportRetryTimer || fTransportRetryTimerBound || fTransportRetryPending || fTransportRetryRequired ||
        fPacketTransport.isEnabled() || fPacketTransport.txPending() || fPacketTransport.rxPending() ||
        fNativeScan || fNativeJoin || fNativeScanState.active || fNativeScanState.draining ||
        fNativeScanState.drainingGeneration || fNativeScanState.submitted || fNativeScanState.cancelling ||
        fNativeScanState.deadline ||
        fNativeJoinState.active || fNativeJoinState.draining || fNativeJoinState.drainingGeneration || fNativeRoam.active || fNativeRoam.draining ||
        fNativeJoinDispatching || fNativeControlOwner || fNativeControlSequence || fNativeObserverReady ||
        fNativeScanState.generation != 1 || fNativeJoinState.generation != 1 || fNativeRoam.generation ||
        fNativeBssGeneration || fNativeAssocReported || fNativeFirstBeaconReported || fNativeConnectReported ||
        fNativeLinkReported || fNativeBssValid || fNativeBssConfigured ||
        fNativeResetPending || fNativeResetQuiescencePending || fNativeJoinAbortPending ||
        fNextNodeToSend || fScanRequestPending || fScanResultWrapping ||
        !_fWorkloop || !_fWorkloop->inGate())
        return false;
    detachControllerReferences();
    fLocalCleanupState = LocalCleanupState::Complete;
    fFailedNativeStartDisposalPrepared = true;
    return true;
}
bool AirportItlwmSkywalkInterface::failedNativeStartDisposalReady() const
{
    return fFailedNativeStartDisposalPrepared && !getProvider() &&
        fSourceInitPhase == SourceInitPhase::Succeeded &&
        fFirstNativeInitPhase == FirstNativeInitPhase::Succeeded && fNativeInitComplete &&
        fNativeIdentityInitComplete && fCustomInitComplete &&
        fNativeStartPhase == NativeStartPhase::Failed && fNativeStopPhase == NativeStopPhase::NotCalled &&
        !fFrameworkStarted && !fStopThread && fLocalCleanupState == LocalCleanupState::Complete &&
        !fLocalCleanupAttempts && !fCleanupDisableComplete &&
        fLocalCleanupFailure == kIOReturnSuccess && !fLocalCleanupReason &&
        !instance && !fHalService && !scanSource && !fNextNodeToSend && !fScanRequestPending &&
        !fScanResultWrapping && !fPacketPool && !fRxCompQueue && !fRxSubQueue &&
        !fTxCompQueue && !fTxSubQueue && !fMCastQueue && !fDatapathProvisioned &&
        !fDatapathEnabled && !fDatapathRetiring && !fDatapathWorkQueue && !fBoundQueueMask &&
        !fRegistrationAttempted && !fRegistrationSucceeded && !fTransportRetryTimer &&
        !fTransportRetryTimerBound && !fTransportRetryPending && !fTransportRetryRequired &&
        !fPacketTransport.isEnabled() && !fPacketTransport.txPending() && !fPacketTransport.rxPending() &&
        !fNativeScan && !fNativeJoin && !fNativeScanState.active && !fNativeScanState.draining &&
        !fNativeScanState.drainingGeneration && !fNativeScanState.submitted &&
        !fNativeScanState.cancelling && !fNativeScanState.deadline &&
        !fNativeJoinState.active && !fNativeJoinState.draining && !fNativeJoinState.drainingGeneration &&
        !fNativeRoam.active && !fNativeRoam.draining && !fNativeJoinDispatching && !fNativeControlOwner &&
        !fNativeObserverReady && !fNativeResetPending && !fNativeResetQuiescencePending && !fNativeJoinAbortPending &&
        !fNativeControlSequence && fNativeScanState.generation == 1 && fNativeJoinState.generation == 1 &&
        !fNativeRoam.generation && !fNativeBssGeneration && !fNativeAssocReported && !fNativeFirstBeaconReported &&
        !fNativeConnectReported && !fNativeLinkReported && !fNativeBssValid && !fNativeBssConfigured;
}

#endif

void AirportItlwmSkywalkInterface::free()
{
    if (fFirstNativeInitPhase == FirstNativeInitPhase::Failed && !fNativeInitComplete) {
        super::free();
        return;
    }
#if __IO80211_TARGET >= __MAC_15_0
    if (failedSourceInitDisposalReady()) {
        super::free();
        return;
    }
#endif
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (failedNativeStartDisposalReady()) {
        super::free();
        return;
    }
#endif
    if (!controllerCleanupReady()) return;
#if __IO80211_TARGET >= __MAC_15_0
    if (fFrameworkStarted || fStopThread)
#else
    if (fFrameworkStarted)
#endif
        stop(getProvider());
    else if (!teardownDatapath())
        return;
#if __IO80211_TARGET >= __MAC_15_0
    // A failed admission must not fall through to native free while started.
    if (fFrameworkStarted || fStopThread) {
        IOLog("AirportItlwmSkywalkInterface::free stop remains pending\n");
        return;
    }
#endif
    if (!datapathTeardownComplete()) return;
    detachControllerReferences();
    super::free();
}

// Copied Skywalk/mbuf transport, with deferred counted completion callbacks.

#if __IO80211_TARGET >= __MAC_15_0
class AirportItlwmPacketOperations {
    AirportItlwmSkywalkInterface &interface;
public:
    struct Descriptor {
        IOMemoryDescriptor *memory;
        UInt8 *bytes;
        UInt64 length;
        explicit operator bool() const { return memory || bytes; }
    };
    explicit AirportItlwmPacketOperations(AirportItlwmSkywalkInterface &value) : interface(value) {}
    bool outputReady() { return interface.instance && interface.instance->nativePacketOutputReady(); }
    UInt32 bufferCount(IOSkywalkPacket *packet) { return packet->IOSkywalkPacket::getPacketBufferCount(); }
    UInt32 packetLength(IOSkywalkPacket *packet) { return packet->IOSkywalkPacket::getDataLength(); }
    UInt16 packetOffset(IOSkywalkPacket *packet) { return packet->IOSkywalkPacket::getDataOffset(); }
    Descriptor descriptor(IOSkywalkPacket *packet) {
        Descriptor view = {};
        view.memory = packet->IOSkywalkPacket::getMemoryDescriptor();
        if (view.memory) {
            view.length = view.memory->getLength();
        } else {
            // Base pool factories do not request per-buffer subdescriptors.
            // This accessor already adds the buffer's memory-segment offset.
            view.bytes = static_cast<UInt8 *>(packet->IOSkywalkPacket::getDataVirtualAddress());
            view.length = packet->IOSkywalkPacket::getBufferSize();
            if (!view.bytes || view.length > UINTPTR_MAX - reinterpret_cast<uintptr_t>(view.bytes))
                view = {};
        }
        return view;
    }
    UInt64 descriptorLength(Descriptor memory) { return memory.length; }
    mbuf_t allocateInput(UInt32 length) {
        return interface.instance->IONetworkController::allocatePacketNoWait(length);
    }
    void freeInput(mbuf_t packet) { mbuf_freem(packet); }
    void submitInput(mbuf_t packet) { interface.instance->submitNativePacket(packet); }
    void markTxDropped(IOSkywalkPacket *packet) {
        // Pool type 1 supplies this native metadata receiver at the same address.
        auto network = reinterpret_cast<IOSkywalkNetworkPacket *>(packet);
        network->IOSkywalkNetworkPacket::setCompletionStatus(EIO);
    }
    bool copyTx(Descriptor memory, UInt64 offset, mbuf_t output, UInt32 length) {
        UInt32 copied = 0;
        for (mbuf_t part = output; part && copied < length; part = mbuf_next(part)) {
            size_t chunk = mbuf_len(part);
            if (chunk > length - copied) chunk = length - copied;
            if (chunk) {
                if (memory.memory) {
                    if (memory.memory->readBytes(offset + copied, mbuf_data(part), chunk) != chunk)
                        return false;
                } else {
                    memcpy(mbuf_data(part), memory.bytes + offset + copied, chunk);
                }
            }
            copied += chunk;
        }
        return copied == length;
    }
    UInt64 inputLength(mbuf_t input) {
        return (mbuf_flags(input) & MBUF_PKTHDR) ? mbuf_pkthdr_len(input) : 0;
    }
    IOSkywalkPacket *allocateRx() {
        IOSkywalkPacket *packet = nullptr;
        // Native option 0x1 uses kern_pbufpool_alloc_nosleep on this workloop.
        IOReturn result = interface.fPacketPool->allocatePacket(&packet, 1);
        return result == kIOReturnSuccess ? packet : nullptr;
    }
    bool prepareRx(IOSkywalkPacket *packet) {
        // A software-produced RX packet owns no RX submission credit.
        // Pool abort completion reads this field independently of prepare's
        // buffer direction, so set it before any partial preparation can fail.
        packet->IOSkywalkPacket::setTransferDirection(2);
        return packet->IOSkywalkPacket::prepareWithQueue(nullptr, 2, 0) == kIOReturnSuccess;
    }
    bool configureRx(IOSkywalkPacket *packet, mbuf_t input, UInt32 length) {
        ether_header_t header = {};
        if (mbuf_copydata(input, 0, sizeof(header), &header) != 0)
            return false;
        UInt8 headerLength = 0;
        auto read = [input](UInt32 offset, UInt8 *bytes, UInt32 count) {
            return mbuf_copydata(input, offset, count, bytes) == 0;
        };
        if (!AirportItlwmPacketTransport<IOSkywalkPacket, mbuf_t, AIRITL_DP_QUEUE_CAPACITY>::
                ethernetHeaderLength(length, read, headerLength))
            return false;
        auto network = reinterpret_cast<IOSkywalkNetworkPacket *>(packet);
        if (packet->IOSkywalkPacket::setDataOffsetAndLength(0, length) != kIOReturnSuccess ||
            network->IOSkywalkNetworkPacket::setHeadroom(0) != kIOReturnSuccess ||
            network->IOSkywalkNetworkPacket::setLinkHeaderLength(headerLength) != kIOReturnSuccess)
            return false;
        bool broadcast = true;
        for (unsigned i = 0; i < ETHER_ADDR_LEN; ++i)
            broadcast = broadcast && header.ether_dhost[i] == 0xff;
        // Native setters set flags, they do not clear them when passed false.
        // Fresh kernel allocation resets packet metadata. No FCS or checksum
        // offload flag is asserted for these software Ethernet frames.
        if (broadcast)
            return network->IOSkywalkNetworkPacket::setIsLinkBroadcast(true) == kIOReturnSuccess;
        if (header.ether_dhost[0] & 1)
            return network->IOSkywalkNetworkPacket::setIsLinkMulticast(true) == kIOReturnSuccess;
        return true;
    }
    bool copyRx(mbuf_t input, Descriptor memory, UInt64 offset, UInt32 length) {
        UInt32 copied = 0;
        for (mbuf_t part = input; part && copied < length; part = mbuf_next(part)) {
            size_t chunk = mbuf_len(part);
            if (chunk > length - copied) chunk = length - copied;
            if (chunk) {
                if (memory.memory) {
                    if (memory.memory->writeBytes(offset + copied, mbuf_data(part), chunk) != chunk)
                        return false;
                } else {
                    memcpy(memory.bytes + offset + copied, mbuf_data(part), chunk);
                }
            }
            copied += chunk;
        }
        return copied == length;
    }
    void returnPacket(IOSkywalkPacket *packet) { interface.fPacketPool->deallocatePacket(packet); }
    void requestTxCompletion() {
        if (interface.fTxCompQueue)
            interface.fTxCompQueue->IOSkywalkTxCompletionQueue::requestEnqueue(nullptr, 1);
    }
    void requestRxCompletion() {
        if (interface.fRxCompQueue)
            interface.fRxCompQueue->IOSkywalkRxCompletionQueue::requestEnqueue(nullptr, 1);
    }
    void requestTxSubmission() {
        if (interface.fTxSubQueue)
            interface.fTxSubQueue->IOSkywalkTxSubmissionQueue::requestDequeue(nullptr, 1);
        if (interface.fMCastQueue && interface.fMCastQueue->IOSkywalkPacketQueue::isQueueReady())
            interface.fMCastQueue->IOSkywalkTxSubmissionQueue::requestDequeue(nullptr, 1);
    }
    void requestTxRetry() {
        if (!(interface.controllerFunctionalReady() && interface.fDatapathEnabled)) return;
        interface.fTransportRetryRequired = true;
        if (interface.fDatapathEnabled && interface.fTransportRetryTimer &&
            !interface.fTransportRetryPending) {
            interface.fTransportRetryPending = true;
            interface.fTransportRetryTimer->setTimeoutMS(10);
        }
    }
};

void AirportItlwmSkywalkInterface::transportRetryAction(OSObject *owner, IOTimerEventSource *timer)
{
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    if (!interface || timer != interface->fTransportRetryTimer) return;
    if (!(interface->controllerFunctionalReady() && interface->fDatapathEnabled)) return;
    interface->fTransportRetryPending = false;
    if (interface->fDatapathEnabled) {
        AirportItlwmPacketOperations operations(*interface);
        operations.requestTxSubmission();
        if (interface->fPacketTransport.txPending()) operations.requestTxCompletion();
        if (interface->fPacketTransport.rxPending()) operations.requestRxCompletion();
        // A native blocked-queue notification can clear backpressure without
        // immediately re-querying capacity. Keep retrying until a positive
        // query proves that networking had another chance to submit.
        if (interface->fTransportRetryRequired) operations.requestTxRetry();
    }
}

bool AirportItlwmSkywalkInterface::nativeTransportAvailable() const
{
    return controllerFunctionalReady() && fFrameworkStarted && fRegistrationSucceeded && fDatapathProvisioned &&
           fPacketPool && fDatapathWorkQueue && fBoundQueueMask == 0x1f;
}

UInt32 AirportItlwmSkywalkInterface::nativeTxFreeSpace()
{
    if (!(controllerFunctionalReady() && fDatapathEnabled)) return 0;
    AirportItlwmPacketOperations operations(*this);
    UInt32 free = operations.outputReady() ? fPacketTransport.txFree() : 0;
    // Diagnostic: the first few blocked queries after enable name the gate.
    static unsigned blockedLogs = 0;
    struct ieee80211com *diagIc = fHalService ? fHalService->get80211Controller() : nullptr;
    if (!free && blockedLogs < 6 && diagIc && diagIc->ic_state == IEEE80211_S_RUN) {
        ++blockedLogs;
        if (instance) instance->logNativeOutputState(operations.outputReady() ? "transport full" : "not ready");
    }
    if (!free) operations.requestTxRetry();
    else fTransportRetryRequired = false;
    return free;
}

UInt32 AirportItlwmSkywalkInterface::submitNativePackets(IOSkywalkPacket *const *packets, UInt32 count)
{
    AirportItlwmPacketOperations operations(*this);
    UInt32 taken = fPacketTransport.submit(packets, count, operations);
    static unsigned submitLogs = 0;
    if (submitLogs < 3) {
        ++submitLogs;
        AIRITL_DBG("AirportItlwm: tx submit count=%u taken=%u\n", count, taken);
    }
    logTransportGuards("submit");
    return taken;
}

UInt32 AirportItlwmSkywalkInterface::completeNativeTx(IOSkywalkPacket **packets, UInt32 count)
{
    AirportItlwmPacketOperations operations(*this);
    UInt32 taken = fPacketTransport.takeTx(packets, count, operations);
    static unsigned completeLogs = 0;
    if (completeLogs < 3 && taken) {
        ++completeLogs;
        AIRITL_DBG("AirportItlwm: tx complete capacity=%u returned=%u pending=%u\n", count, taken,
              fPacketTransport.txPending());
    }
    logTransportGuards("complete");
    return taken;
}

void AirportItlwmSkywalkInterface::logTransportGuards(const char *where)
{
    // Report each change of the duplicate/nesting guard counters, at most 12 lines per boot.
    static UInt32 lastSubmit = 0, lastComplete = 0, lastNested = 0;
    static unsigned guardLogs = 0;
    UInt32 dupSubmit = fPacketTransport.duplicateSubmitCount();
    UInt32 dupComplete = fPacketTransport.duplicateCompletionCount();
    UInt32 nested = fPacketTransport.nestedCallCount();
    if (dupSubmit == lastSubmit && dupComplete == lastComplete && nested == lastNested) return;
    lastSubmit = dupSubmit; lastComplete = dupComplete; lastNested = nested;
    if (guardLogs >= 12) return;
    ++guardLogs;
    IOLog("AirportItlwm: tx guard (%s) duplicateSubmit=%u duplicateComplete=%u nested=%u pending=%u\n",
          where, dupSubmit, dupComplete, nested, fPacketTransport.txPending());
}

UInt32 AirportItlwmSkywalkInterface::completeNativeRx(IOSkywalkPacket **packets, UInt32 count)
{
    AirportItlwmPacketOperations operations(*this);
    return fPacketTransport.takeRx(packets, count, operations);
}

IOReturn AirportItlwmSkywalkInterface::inputNativeAction(OSObject *owner, void *input, void *, void *, void *)
{
    auto interface = static_cast<AirportItlwmSkywalkInterface *>(owner);
    AirportItlwmPacketOperations operations(*interface);
    static unsigned receiveLogs = 0;
    if (receiveLogs < 3) {
        ++receiveLogs;
        AIRITL_DBG("AirportItlwm: rx input len=%zu enabled=%d\n", mbuf_pkthdr_len(static_cast<mbuf_t>(input)),
              interface->fDatapathEnabled);
    }
    interface->fPacketTransport.receive(static_cast<mbuf_t>(input), operations);
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::inputNativePacket(void *context, mbuf_t packet)
{
    auto interface = static_cast<AirportItlwmSkywalkInterface *>(context);
    if (!packet) return;
    if (!interface || !interface->fDatapathWorkQueue ||
        interface->fDatapathWorkQueue->runAction(inputNativeAction, interface, packet) != kIOReturnSuccess)
        mbuf_freem(packet);
}

IOReturn AirportItlwmSkywalkInterface::outputNativeAction(OSObject *owner, void *, void *, void *, void *)
{
    auto interface = static_cast<AirportItlwmSkywalkInterface *>(owner);
    if (interface->fDatapathEnabled) {
        AirportItlwmPacketOperations operations(*interface);
        operations.requestTxSubmission();
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::outputNativeReady(void *context)
{
    auto interface = static_cast<AirportItlwmSkywalkInterface *>(context);
    if (interface && interface->fDatapathWorkQueue)
        interface->fDatapathWorkQueue->runAction(outputNativeAction, interface);
}
#endif

// Callback results are packet counts. Queue work already holds the shared gate.
static UInt32 airitl_tx_query_free_space(OSObject *owner, IOSkywalkTxSubmissionQueue *q, UInt32 *outSpace)
{
#if __IO80211_TARGET >= __MAC_15_0
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    UInt32 packets = interface ? interface->nativeTxFreeSpace() : 0;
    // The query returns packet capacity and writes a separate byte budget.
    // Native packetSubmission clamps both before obtaining networking packets.
    if (outSpace) *outSpace = packets * AIRITL_DP_BUFFER_SIZE;
    return packets;
#else
    if (outSpace) *outSpace = AIRITL_DP_QUEUE_CAPACITY;
    return 0;
#endif
}
// Array mode: see IOSkywalkPacketQueues.h. The const action selects the base factory without list mode.
static UInt32 airitl_tx_submit(OSObject *owner, IOSkywalkTxSubmissionQueue *q, IOSkywalkPacket *const *pkts, UInt32 count, void *refCon)
{
#if __IO80211_TARGET >= __MAC_15_0
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    return interface ? interface->submitNativePackets(pkts, count) : 0;
#else
    return 0;
#endif
}
static UInt32 airitl_tx_complete(OSObject *owner, IOSkywalkTxCompletionQueue *q, IOSkywalkPacket **pkts, UInt32 count, void *refCon)
{
#if __IO80211_TARGET >= __MAC_15_0
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    return interface ? interface->completeNativeTx(pkts, count) : 0;
#else
    return 0;
#endif
}
static UInt32 airitl_rx_submit(OSObject *owner, IOSkywalkRxSubmissionQueue *q, IOSkywalkPacket **pkts, UInt32 count, void *refCon)
{
    return 0;
}
static UInt32 airitl_rx_complete(OSObject *owner, IOSkywalkRxCompletionQueue *q, IOSkywalkPacket **pkts, UInt32 count, void *refCon)
{
#if __IO80211_TARGET >= __MAC_15_0
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    return interface ? interface->completeNativeRx(pkts, count) : 0;
#else
    return 0;
#endif
}

bool AirportItlwmSkywalkInterface::
provisionDatapath(IOMapper *mapper)
{
    if (!controllerFunctionalReady())
        return false;
    if (fDatapathProvisioned)
        return true;

    IOSkywalkPacketBufferPool::PoolOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.packetCount        = AIRITL_DP_QUEUE_CAPACITY;
    opts.bufferCount        = AIRITL_DP_QUEUE_CAPACITY;
    opts.bufferSize         = AIRITL_DP_BUFFER_SIZE;
    opts.maxBuffersPerPacket = AIRITL_DP_MAX_BUFFERS_PER_PACKET;
    opts.memorySegmentSize  = AIRITL_DP_BUFFER_SIZE;
    opts.poolFlags          = 0;
#if __IO80211_TARGET >= __MAC_15_0
    // Type 0 sets KBIF_QUANTUM, which network-interface nexus pools forbid.
    const UInt32 packetType = 1;
#else
    const UInt32 packetType = 0;
#endif

#ifdef AIRITL_DP_DMA
    // A DMA-capable pool whose segments carry an IODMACommand built from the iwx device
    // mapper, so the framework's nexus-connect can map the buffers into the NIC IOVM (the missing
    // piece that faulted ~4s after airportd enabled the datapath).
    AIRITL_DBG("AirportItlwm: provisioning DMA pool (mapper=%p)\n", mapper);
    fPacketPool = AirportItlwmSkywalkPacketPool::withMapperName("AirportItlwmP0Pool", this,
                                                                packetType, &opts, mapper);
#else
    (void)mapper;
    fPacketPool = IOSkywalkPacketBufferPool::withName("AirportItlwmP0Pool", this,
                                                      packetType, &opts);
#endif
    if (!fPacketPool) {
        XYLog("%s packet pool create FAIL\n", __FUNCTION__);
        IOLog("AirportItlwm: packet buffer pool creation failed\n");
        return false;
    }

    fTxSubQueue = IOSkywalkTxSubmissionQueue::withPool(fPacketPool, AIRITL_DP_QUEUE_CAPACITY, 0,
                                                       this, airitl_tx_query_free_space, airitl_tx_submit,
                                                       this, 1 /*options*/, 0 /*serviceClass*/);
    fTxCompQueue = IOSkywalkTxCompletionQueue::withPool(fPacketPool, AIRITL_DP_QUEUE_CAPACITY, 1,
                                                        this, airitl_tx_complete, this, 0);
    // Native RX init rejects a buffer count above the pool maximum before
    // allocating its packet table. Use the same count for the pool and queue.
    fRxSubQueue = IOSkywalkRxSubmissionQueue::withPool(fPacketPool, AIRITL_DP_QUEUE_CAPACITY,
                                                       AIRITL_DP_MAX_BUFFERS_PER_PACKET,
                                                       2 /*queueId*/, this, airitl_rx_submit, this, 0);
    fRxCompQueue = IOSkywalkRxCompletionQueue::withPool(fPacketPool, AIRITL_DP_QUEUE_CAPACITY, 3,
                                                        this, airitl_rx_complete, this, 0);
    // Retain the separately owned diagnostic multicast queue. On Sequoia the
    // multicast getter returns the registered TX queue; compatibility with all
    // downstream multicast consumers remains unverified.
    fMCastQueue = IOSkywalkTxSubmissionQueue::withPool(fPacketPool, AIRITL_DP_QUEUE_CAPACITY, 4,
                                                       this, airitl_tx_query_free_space, airitl_tx_submit,
                                                       this, 1 /*options*/, 0 /*serviceClass*/);
    if (!fTxSubQueue || !fTxCompQueue || !fRxSubQueue || !fRxCompQueue || !fMCastQueue) {
        XYLog("%s queue create FAIL tx=%p txc=%p rx=%p rxc=%p\n", __FUNCTION__,
              fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue);
        IOLog("AirportItlwm: queue creation failed\n");
        if (!teardownDatapath())
            IOLog("AirportItlwm: failed provisioning retains incomplete datapath\n");
        return false;
    }

    fDatapathProvisioned = true;
    AIRITL_DBG("AirportItlwm: datapath provisioned (pool=%p txq=%p txc=%p rxq=%p rxc=%p)\n",
          fPacketPool, fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue);

    return true;
}

// Bind queue event sources before the canonical registration creates its link.
IOReturn AirportItlwmSkywalkInterface::
registerDatapath(IOSkywalkEthernetInterface::RegistrationInfo *reginfo)
{
    if (!controllerFunctionalReady() || !fFrameworkStarted || !fDatapathProvisioned || !fPacketPool || !reginfo || !instance)
        return kIOReturnNotReady;
    if (fRegistrationAttempted || fDatapathWorkQueue)
        return kIOReturnBusy;

    IO80211WorkQueue *workQueue = instance->getWorkQueue();
    if (!workQueue)
        return kIOReturnNotReady;
    fDatapathWorkQueue = workQueue;
    workQueue->retain();
#if __IO80211_TARGET >= __MAC_15_0
    fTransportRetryTimer = IOTimerEventSource::timerEventSource(this, transportRetryAction);
    if (!fTransportRetryTimer)
        return kIOReturnNoMemory;
    IOReturn timerResult = workQueue->addEventSource(fTransportRetryTimer);
    if (timerResult != kIOReturnSuccess)
        return timerResult;
    fTransportRetryTimerBound = true;
    fTransportRetryTimer->disable();
#endif
    IOSkywalkPacketQueue *allQueues[5] = {
        fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue, fMCastQueue
    };
    for (unsigned int i = 0; i < 5; i++) {
        // Factory objects are IOEventSources. The minimal local queue declaration
        // does not express that base, so check the runtime type before binding.
        IOEventSource *source = OSDynamicCast(IOEventSource, allQueues[i]);
        if (!source)
            return kIOReturnBadArgument;
        IOReturn result = workQueue->addEventSource(source);
        if (result != kIOReturnSuccess)
            return result;
        fBoundQueueMask |= 1U << i;
    }
    IOSkywalkPacketQueue *queues[4] = {
        fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue
    };
    // The framework may allocate private records before returning an error.
    fRegistrationAttempted = true;
    IOReturn result = registerEthernetInterface(reginfo, queues, 4, fPacketPool, fPacketPool, 0);
#if __IO80211_TARGET >= __MAC_15_0
    fRegistrationSucceeded = result == kIOReturnSuccess;
#endif
    IOLog("AirportItlwm: registerEthernetInterface(q=4 pool=%p) -> 0x%x\n", fPacketPool, result);
    return result;
}

bool AirportItlwmSkywalkInterface::failLocalCleanup(const char *reason, IOReturn result,
                                                    bool admissionDeferred)
{
    fLocalCleanupFailure = result;
    fLocalCleanupReason = reason;
    fLocalCleanupState = admissionDeferred && fLocalCleanupAttempts < kMaxLocalCleanupAttempts ?
        LocalCleanupState::Pending : LocalCleanupState::Exhausted;
    IOLog("AirportItlwm: local cleanup %s attempt %u/%u result 0x%x state %u\n",
          reason, fLocalCleanupAttempts, kMaxLocalCleanupAttempts, result,
          static_cast<unsigned>(fLocalCleanupState));
    return false;
}

bool AirportItlwmSkywalkInterface::validateLocalCleanup()
{
    if (!controllerCleanupReady() || (fBoundQueueMask & ~0x1fU))
        return failLocalCleanup("invalid phase or queue mask", kIOReturnBadArgument);
#if __IO80211_TARGET >= __MAC_15_0
    if ((fTransportRetryTimer || fTransportRetryTimerBound || fTransportRetryPending ||
         fTransportRetryRequired) && !fDatapathWorkQueue)
        return failLocalCleanup("missing retry queue", kIOReturnBadArgument);
    if (fRegistrationSucceeded && !fRegistrationAttempted)
        return failLocalCleanup("inconsistent registration", kIOReturnBadArgument);
    if (fTransportRetryTimer && fTransportRetryTimer->getWorkLoop() &&
        (!fTransportRetryTimerBound || fTransportRetryTimer->getWorkLoop() != fDatapathWorkQueue))
        return failLocalCleanup("foreign or unrecorded retry binding", kIOReturnBadArgument);
    if (fTransportRetryTimerBound && (!fTransportRetryTimer || !fDatapathWorkQueue ||
        (fTransportRetryTimer->getWorkLoop() &&
         fTransportRetryTimer->getWorkLoop() != fDatapathWorkQueue)))
        return failLocalCleanup("invalid retry timer binding", kIOReturnBadArgument);
#endif
    if (fRegistrationAttempted && !fDatapathWorkQueue)
        return failLocalCleanup("missing registration queue", kIOReturnBadArgument);
    IOSkywalkPacketQueue *queues[5] = {
        fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue, fMCastQueue
    };
    for (unsigned i = 0; i < 5; ++i) {
        auto source = OSDynamicCast(IOEventSource, queues[i]);
        if (source && source->getWorkLoop() &&
            source->getWorkLoop() != fDatapathWorkQueue)
            return failLocalCleanup("foreign queue loop", kIOReturnBadArgument);
        if (!(fBoundQueueMask & (1U << i))) {
            if (source && source->getWorkLoop())
                return failLocalCleanup("unrecorded queue binding", kIOReturnBadArgument);
            continue;
        }
        if (!source || !fDatapathWorkQueue)
            return failLocalCleanup("invalid queue binding", kIOReturnBadArgument);
    }
    return true;
}

bool AirportItlwmSkywalkInterface::beginLocalCleanup()
{
    if (fLocalCleanupState == LocalCleanupState::InProgress ||
        fLocalCleanupState == LocalCleanupState::Complete ||
        fLocalCleanupState == LocalCleanupState::Exhausted) return false;
    // Leaving Idle closes functional admission even if structure is invalid.
    fLocalCleanupState = LocalCleanupState::InProgress;
    if (!validateLocalCleanup()) return false;
    if (fLocalCleanupAttempts >= kMaxLocalCleanupAttempts)
        return failLocalCleanup("attempt budget exhausted", kIOReturnNotReady);
    ++fLocalCleanupAttempts;
    return true;
}

#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmSkywalkInterface::cleanupDisableGatedAction(OSObject *owner,
    void *entered, void *, void *, void *)
{
    *static_cast<bool *>(entered) = true;
    return setNativeDatapathAction(owner, nullptr, nullptr, nullptr, nullptr);
}
#endif

bool AirportItlwmSkywalkInterface::disableForCleanup()
{
    if (fCleanupDisableComplete) return true;
#if __IO80211_TARGET >= __MAC_15_0
    bool entered = false;
    IOReturn result = kIOReturnSuccess;
    if (fDatapathWorkQueue)
        result = fDatapathWorkQueue->runAction(cleanupDisableGatedAction, this, &entered);
    else
        fDatapathEnabled = false;
    if (result != kIOReturnSuccess)
        return failLocalCleanup(entered ? "disable operation failed" : "disable admission deferred",
                                result, !entered);
#else
    // Older targets have no gated transport drain. Preserve their concrete
    // source disable step inside this once-only cleanup checkpoint.
    IOSkywalkPacketQueue *queues[5] = {
        fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue, fMCastQueue
    };
    for (unsigned i = 0; i < 5; ++i) {
        if (!(fBoundQueueMask & (1U << i))) continue;
        IOEventSource *source = OSDynamicCast(IOEventSource, queues[i]);
        source->disable();
    }
#endif
    fDatapathEnabled = false;
    fCleanupDisableComplete = true;
    return true;
}

bool AirportItlwmSkywalkInterface::teardownDatapath()
{
    if (fLocalCleanupState == LocalCleanupState::Complete)
        return datapathTeardownComplete();
    // Started children must discharge their stop obligation through legal stop.
    if (fFrameworkStarted || fNativeStopPhase == NativeStopPhase::InProgress) return false;
    if (!beginLocalCleanup()) return false;
    return finishDatapathCleanup();
}

bool AirportItlwmSkywalkInterface::finishDatapathCleanup()
{
    if (fLocalCleanupState != LocalCleanupState::InProgress) return false;
    if (!validateLocalCleanup() || !disableForCleanup()) return false;
    if (fFrameworkStarted || fNativeStopPhase == NativeStopPhase::InProgress ||
        (fNativeStartPhase == NativeStartPhase::Succeeded &&
         fNativeStopPhase != NativeStopPhase::Returned))
        return failLocalCleanup("native stop still owed", kIOReturnNotReady);
    IOSkywalkPacketQueue *queues[5] = {
        fTxSubQueue, fTxCompQueue, fRxSubQueue, fRxCompQueue, fMCastQueue
    };
    for (unsigned int i = 0; i < 5; i++) {
        if (!(fBoundQueueMask & (1U << i))) continue;
        IOEventSource *source = OSDynamicCast(IOEventSource, queues[i]);
        IOReturn result = fDatapathWorkQueue->removeEventSource(source);
        if (result != kIOReturnSuccess)
            return failLocalCleanup("queue removal failed", result);
        fBoundQueueMask &= ~(1U << i);
    }
#if __IO80211_TARGET >= __MAC_15_0
    // The successful disable checkpoint already cancelled and disabled retry.
    if (fTransportRetryTimerBound) {
        IOReturn result = fDatapathWorkQueue->removeEventSource(fTransportRetryTimer);
        if (result != kIOReturnSuccess)
            return failLocalCleanup("retry timer removal failed", result);
        fTransportRetryTimerBound = false;
    }
#endif
    // Native deregistration completion remains separate from stop-call return.
    if (fRegistrationAttempted) {
        IOReturn result = deregisterEthernetInterface(0);
        if (result != kIOReturnSuccess)
            return failLocalCleanup("deregistration failed", result);
        fRegistrationAttempted = false;
#if __IO80211_TARGET >= __MAC_15_0
        fRegistrationSucceeded = false;
#endif
    }
#if __IO80211_TARGET >= __MAC_15_0
    IOTimerEventSource *retryTimer = fTransportRetryTimer;
    IOSkywalkRxCompletionQueue *rxCompletionQueue = fRxCompQueue;
    IOSkywalkRxSubmissionQueue *rxSubmissionQueue = fRxSubQueue;
    IOSkywalkTxCompletionQueue *txCompletionQueue = fTxCompQueue;
    IOSkywalkTxSubmissionQueue *txSubmissionQueue = fTxSubQueue;
    IOSkywalkTxSubmissionQueue *multicastQueue = fMCastQueue;
    IOSkywalkPacketBufferPool *packetPool = fPacketPool;
    IO80211WorkQueue *workQueue = fDatapathWorkQueue;
    fTransportRetryTimer = nullptr;
    fRxCompQueue = nullptr;
    fRxSubQueue = nullptr;
    fTxCompQueue = nullptr;
    fTxSubQueue = nullptr;
    fMCastQueue = nullptr;
    fPacketPool = nullptr;
    fDatapathWorkQueue = nullptr;
    fDatapathProvisioned = false;
    if (retryTimer) retryTimer->release();
    if (rxCompletionQueue) rxCompletionQueue->release();
    if (rxSubmissionQueue) rxSubmissionQueue->release();
    if (txCompletionQueue) txCompletionQueue->release();
    if (txSubmissionQueue) txSubmissionQueue->release();
    if (multicastQueue) multicastQueue->release();
    if (packetPool) packetPool->release();
    if (workQueue) workQueue->release();
#else
    OSSafeReleaseNULL(fRxCompQueue);
    OSSafeReleaseNULL(fRxSubQueue);
    OSSafeReleaseNULL(fTxCompQueue);
    OSSafeReleaseNULL(fTxSubQueue);
    OSSafeReleaseNULL(fMCastQueue);
    OSSafeReleaseNULL(fPacketPool);
    OSSafeReleaseNULL(fDatapathWorkQueue);
    fDatapathProvisioned = false;
#endif
    fLocalCleanupState = LocalCleanupState::Complete;
    fLocalCleanupFailure = kIOReturnSuccess;
    fLocalCleanupReason = nullptr;
    return true;
}

// --- datapath getter overrides: return provisioned objects instead of inherited null stubs ---
void *AirportItlwmSkywalkInterface::getTxPacketPool(void)            { return fPacketPool; }
void *AirportItlwmSkywalkInterface::getRxPacketPool(void)            { return fPacketPool; }
void *AirportItlwmSkywalkInterface::getTxCompQueue(void)            { return fTxCompQueue; }
void *AirportItlwmSkywalkInterface::getRxCompQueue(void)            { return fRxCompQueue; }
void *AirportItlwmSkywalkInterface::getTxSubQueue(apple80211_wme_ac) { return fTxSubQueue; }
#if __IO80211_TARGET >= __MAC_15_0
void *AirportItlwmSkywalkInterface::getMultiCastQueue(void)
{
    // Borrow the registered ring. Multicast and broadcast Ethernet frames use
    // the same copied TX path and completion ownership as unicast frames.
    // fMCastQueue remains a separately owned diagnostic object.
    return fTxSubQueue;
}
#else
void *AirportItlwmSkywalkInterface::getMultiCastQueue(void)         { return fMCastQueue; }
#endif
int   AirportItlwmSkywalkInterface::getNumTxQueues(void)            { return 1; }
UInt64 AirportItlwmSkywalkInterface::getTxQueueDepth(void)          { return AIRITL_DP_QUEUE_CAPACITY; }
UInt64 AirportItlwmSkywalkInterface::getRxQueueCapacity(void)       { return AIRITL_DP_QUEUE_CAPACITY; }
int   AirportItlwmSkywalkInterface::getTxHeadroom(void)             { return 0; }
#if __IO80211_TARGET >= __MAC_15_0
UInt AirportItlwmSkywalkInterface::getHardwareAssists(void)
{
    // The copy adapter carries complete Ethernet bytes and no offload metadata.
    // Inherited getTSOOptions also returns kIOReturnUnsupported on this host.
    return 0;
}

IOReturn AirportItlwmSkywalkInterface::setNativeDatapathAction(OSObject *owner, void *requested, void *, void *, void *)
{
    auto interface = static_cast<AirportItlwmSkywalkInterface *>(owner);
    AirportItlwmPacketOperations operations(*interface);
    IOSkywalkPacketQueue *queues[5] = {interface->fTxCompQueue, interface->fRxCompQueue,
        interface->fTxSubQueue, interface->fRxSubQueue, interface->fMCastQueue};
    if (!requested) {
        interface->fDatapathEnabled = false;
        interface->fTransportRetryPending = false;
        interface->fTransportRetryRequired = false;
        if (interface->fTransportRetryTimer) {
            interface->fTransportRetryTimer->cancelTimeout();
            interface->fTransportRetryTimer->disable();
        }
        // The gate excludes queue callbacks. Return owned prepared packets while
        // their source queues still exist, then disable their asynchronous work.
        interface->fPacketTransport.disableAndDrain(operations);
        for (auto queue : queues) {
            IOEventSource *source = queue ? OSDynamicCast(IOEventSource, queue) : nullptr;
            if (source) source->disable();
        }
        return kIOReturnSuccess;
    }
    if (interface->fDatapathRetiring) {
        IOLog("AirportItlwm: datapath enable refused: retiring\n");
        return kIOReturnNotReady;
    }
    if (!interface->nativeTransportAvailable()) {
        IOLog("AirportItlwm: datapath enable refused: ctl=%d fw=%d reg=%d prov=%d pool=%d wq=%d mask=0x%x\n",
              interface->controllerFunctionalReady(), interface->fFrameworkStarted,
              interface->fRegistrationSucceeded, interface->fDatapathProvisioned,
              interface->fPacketPool != nullptr, interface->fDatapathWorkQueue != nullptr,
              interface->fBoundQueueMask);
        return kIOReturnNotReady;
    }
    // The used queues must have their native backing rings initialized.
    // The separate multicast getter has no registered ring of its own; enable
    // it only if the framework has supplied one. Network multicast frames can
    // also arrive on the registered TX submission ring.
    for (unsigned i = 0; i < 3; ++i) {
        IOEventSource *source = OSDynamicCast(IOEventSource, queues[i]);
        if (!source || source->getWorkLoop() != interface->fDatapathWorkQueue ||
            !queues[i]->IOSkywalkPacketQueue::isQueueReady()) {
            IOLog("AirportItlwm: datapath enable refused: queue %u src=%d loop=%d ready=%d\n", i,
                  source != nullptr, source && source->getWorkLoop() == interface->fDatapathWorkQueue,
                  queues[i] && queues[i]->IOSkywalkPacketQueue::isQueueReady());
            return kIOReturnNotReady;
        }
    }
    // Keep the packet transport and its rings intact. Native TX enable must
    // observe zero source capacity before the original enabled commit below.
    interface->fDatapathEnabled = false;
    for (unsigned i = 0; i < 5; ++i) {
        // RX input is copied into independent pool allocations. Do not enable
        // the hardware refill ring or reserve its RX submission credits.
        if (i == 3) continue;
        auto queue = queues[i];
        if (queue && queue->IOSkywalkPacketQueue::isQueueReady()) {
            IOEventSource *source = OSDynamicCast(IOEventSource, queue);
            if (source) source->enable();
        }
    }
    interface->fDatapathEnabled = true;
    if (interface->fTransportRetryTimer) interface->fTransportRetryTimer->enable();
    interface->fPacketTransport.enable(operations);
    AIRITL_DBG("AirportItlwm: datapath enabled\n");
    return kIOReturnSuccess;
}

struct AiritlDatapathRetirementWitness {
    IO80211WorkQueue *workQueue;
    bool entered;
    bool completed;
    IOReturn disabled;
};

IOReturn AirportItlwmSkywalkInterface::retireDatapathAction(OSObject *owner,
    void *context, void *, void *, void *)
{
    auto interface = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    auto witness = static_cast<AiritlDatapathRetirementWitness *>(context);
    if (!interface || !witness || !witness->workQueue ||
        interface->fDatapathWorkQueue != witness->workQueue ||
        !witness->workQueue->inGate() || !interface->controllerCleanupReady())
        return kIOReturnNotReady;
    witness->entered = true;
    interface->fDatapathRetiring = true;
    witness->disabled = setNativeDatapathAction(owner, nullptr, nullptr, nullptr, nullptr);
    witness->completed = witness->disabled == kIOReturnSuccess;
    return witness->disabled;
}

IOReturn AirportItlwmSkywalkInterface::retireDatapath()
{
    if (!controllerCleanupReady()) return kIOReturnNotReady;
    // The only caller is final controller quiescence. It holds this child and
    // its dependencies, closes start and owns the serialized cleanup phase.
    IO80211WorkQueue *workQueue = fDatapathWorkQueue;
    if (!workQueue) {
        if (fBoundQueueMask || fRegistrationAttempted || fRegistrationSucceeded ||
            fTransportRetryTimerBound || fTransportRetryPending || fTransportRetryRequired ||
            fDatapathEnabled || fPacketTransport.isEnabled() ||
            fPacketTransport.txPending() || fPacketTransport.rxPending())
            return kIOReturnNotReady;
        // This exclusive no-work path neither calls native code nor releases
        // allocated, unbound objects. Only fresh initialization clears the latch.
        fDatapathRetiring = true;
        fDatapathEnabled = false;
        return kIOReturnSuccess;
    }
    // Capture the existing owned original W. A provider lookup is not a lease.
    workQueue->retain();
    AiritlDatapathRetirementWitness witness{workQueue, false, false, kIOReturnNotReady};
    IOReturn result = workQueue->runAction(retireDatapathAction, this, &witness);
    workQueue->release();
    if (result != kIOReturnSuccess) return result;
    if (!witness.entered) return kIOReturnNotReady;
    if (witness.disabled != kIOReturnSuccess) return witness.disabled;
    return witness.completed ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::enableDatapath(void)
{
    if (!fDatapathWorkQueue) return kIOReturnNotReady;
    return fDatapathWorkQueue->runAction(setNativeDatapathAction, this, this);
}

IOReturn AirportItlwmSkywalkInterface::enable(UInt options)
{
    IOReturn result = super::enable(options);
    // Same order as AppleBCMWLANSkywalkInterface::enable: base first, then the
    // driver's own datapath. A refusal is logged and does not fail the netif.
    IOReturn datapath = result == kIOReturnSuccess ? enableDatapath() : kIOReturnNotReady;
    AIRITL_DBG("AirportItlwm: netif enable(0x%x) -> 0x%x, enableDatapath -> 0x%x\n",
          (unsigned)options, result, datapath);
    return result;
}

IOReturn AirportItlwmSkywalkInterface::disable(UInt options)
{
    IOReturn datapath = disableDatapath();
    IOReturn result = super::disable(options);
    AIRITL_DBG("AirportItlwm: netif disable(0x%x) -> 0x%x, disableDatapath -> 0x%x\n",
          (unsigned)options, result, datapath);
    return result;
}

IOReturn AirportItlwmSkywalkInterface::disableDatapath(void)
{
    if (fDatapathWorkQueue)
        return fDatapathWorkQueue->runAction(setNativeDatapathAction, this, nullptr);
    // No queue was bound, so no callback or native packet could be acquired.
    fDatapathEnabled = false;
    return kIOReturnSuccess;
}
#else
void AirportItlwmSkywalkInterface::enableDatapath(void) { if (controllerFunctionalReady()) fDatapathEnabled = true; }
void AirportItlwmSkywalkInterface::disableDatapath(void) { fDatapathEnabled = false; }
#endif

void AirportItlwmSkywalkInterface::
detachControllerReferences()
{
    instance = NULL;
    fHalService = NULL;
    scanSource = NULL;
    fNextNodeToSend = NULL;
    fScanResultWrapping = false;
    fScanRequestPending = 0;
}

//ifnet_t AirportItlwmSkywalkInterface::
//getBSDInterface()
//{
//    if (instance->bsdInterface)
//        return instance->bsdInterface->getIfnet();
//    return NULL;
//}

IOReturn AirportItlwmSkywalkInterface::
getSSID(struct apple80211_ssid_data *sd)
{
    if (!sd)
        return kIOReturnError;

    memset(sd, 0, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com * ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        sd->ssid_len = MIN((uint32_t)ic->ic_bss->ni_esslen, (uint32_t)sizeof(sd->ssid_bytes));
        if (sd->ssid_len != 0)
            memcpy(sd->ssid_bytes, ic->ic_bss->ni_essid, sd->ssid_len);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    if (!ad)
        return kIOReturnBadArgument;

    memset(ad, 0, sizeof(*ad));
#if __IO80211_TARGET < __MAC_15_0
    ad->version = APPLE80211_VERSION;
#endif
    ad->authtype_lower = current_authtype_lower;
    ad->authtype_upper = current_authtype_upper;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    if (!ad)
        return kIOReturnBadArgument;

#if __IO80211_TARGET >= __MAC_15_0
    if (ad->authtype_selector || ad->authtype_lower != APPLE80211_AUTHTYPE_OPEN ||
        (ad->authtype_upper & ~(APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK |
                                APPLE80211_AUTHTYPE_SHA256_PSK))) return kIOReturnUnsupported;
#endif
    current_authtype_lower = ad->authtype_lower;
    current_authtype_upper = ad->authtype_upper;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setCIPHER_KEY(struct apple80211_key *key)
{
#if __IO80211_TARGET >= __MAC_15_0
    return setNativeCipherKey(key);
#else
    if (!key || key->key_len > APPLE80211_KEY_BUFF_LEN || key->key_rsc_len > APPLE80211_RSC_LEN)
        return kIOReturnBadArgument;

    XYLog("%s\n", __FUNCTION__);
    static_assert(__offsetof(struct apple80211_key, key_ea) == 92, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, key_rsc_len) == 80, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kck_len) == 100, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kek_len) == 120, "struct corrupted");
    static_assert(__offsetof(struct apple80211_key, wowl_kck_key) == 104, "struct corrupted");
    XYLog("Set key request: len=%d cipher_type=%d flags=%d index=%d rsc_len=%d\n",
          key->key_len, key->key_cipher_type, key->key_flags, key->key_index, key->key_rsc_len);

    switch (key->key_cipher_type) {
        case APPLE80211_CIPHER_NONE:
            // clear existing key
//            XYLog("Setting NONE key is not supported\n");
            break;
        case APPLE80211_CIPHER_WEP_40:
        case APPLE80211_CIPHER_WEP_104:
            XYLog("Setting WEP key %d is not supported\n", key->key_index);
            return kIOReturnUnsupported;
        case APPLE80211_CIPHER_TKIP:
        case APPLE80211_CIPHER_AES_OCB:
        case APPLE80211_CIPHER_AES_CCM:
            switch (key->key_flags) {
                case 4: // PTK
                    return setPTK(key->key, key->key_len);
                case 0: // GTK
                    return setGTK(key->key, key->key_len, key->key_index, key->key_rsc);
                default:
                    XYLog("unsupported cipher key flags: %d\n", key->key_flags);
                    return kIOReturnBadArgument;
            }
        case APPLE80211_CIPHER_PMK:
            XYLog("Setting WPA PMK is not supported\n");
            return kIOReturnUnsupported;
        case APPLE80211_CIPHER_MSK: {
            XYLog("Setting MSK\n");
            if (!fHalService)
                return kIOReturnNotAttached;
            struct ieee80211com *msk_ic = fHalService->get80211Controller();
            if (!msk_ic || !msk_ic->ic_bss)
                return kIOReturnNotAttached;
            ieee80211_pmksa_add(msk_ic, IEEE80211_AKM_8021X,
                                msk_ic->ic_bss->ni_macaddr, key->key, 0);
            break;
        }
        case APPLE80211_CIPHER_PMKSA: {
            XYLog("Setting WPA PMKSA\n");
            if (!fHalService)
                return kIOReturnNotAttached;
            struct ieee80211com *pmksa_ic = fHalService->get80211Controller();
            if (!pmksa_ic || !pmksa_ic->ic_bss)
                return kIOReturnNotAttached;
            ieee80211_pmksa_add(pmksa_ic, IEEE80211_AKM_8021X,
                                pmksa_ic->ic_bss->ni_macaddr, key->key, 0);
            break;
        }
        default:
            XYLog("unsupported cipher type: %d\n", key->key_cipher_type);
            return kIOReturnUnsupported;
    }
    //fInterface->postMessage(APPLE80211_M_CIPHER_KEY_CHANGED);
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwmSkywalkInterface::
getPHY_MODE(struct apple80211_phymode_data *pd)
{
    if (!pd)
        return kIOReturnError;

    memset(pd, 0, sizeof(*pd));
    pd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    pd->phy_mode = APPLE80211_MODE_11A
    | APPLE80211_MODE_11B
    | APPLE80211_MODE_11G
    | APPLE80211_MODE_11N;
    
    if (ic->ic_flags & IEEE80211_F_VHTON)
        pd->phy_mode |= APPLE80211_MODE_11AC;
    
    if (ic->ic_flags & IEEE80211_F_HEON)
        pd->phy_mode |= APPLE80211_MODE_11AX;
    
    switch (ic->ic_curmode) {
        case IEEE80211_MODE_AUTO:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
        case IEEE80211_MODE_11A:
            pd->active_phy_mode = APPLE80211_MODE_11A;
            break;
        case IEEE80211_MODE_11B:
            pd->active_phy_mode = APPLE80211_MODE_11B;
            break;
        case IEEE80211_MODE_11G:
            pd->active_phy_mode = APPLE80211_MODE_11G;
            break;
        case IEEE80211_MODE_11N:
            pd->active_phy_mode = APPLE80211_MODE_11N;
            break;
        case IEEE80211_MODE_11AC:
            pd->active_phy_mode = APPLE80211_MODE_11AC;
            break;
        case IEEE80211_MODE_11AX:
            pd->active_phy_mode = APPLE80211_MODE_11AX;
            break;
            
        default:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
    }
    
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getCHANNEL(struct apple80211_channel_data *cd)
{
    if (!cd)
        return kIOReturnError;

    memset(cd, 0, sizeof(*cd));
    cd->version = APPLE80211_VERSION;
    cd->channel.version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com * ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss && ic->ic_bss->ni_chan) {
        cd->channel.channel = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
        cd->channel.flags = ieeeChanFlag2apple(ic->ic_bss->ni_chan->ic_flags, ic->ic_bss->ni_chw);
        return kIOReturnSuccess;
    }
    // Apple getCHANNEL answers from the firmware's
    // current chanspec when not associated; ours is the node's current channel.
    if (ic && ic->ic_bss && ic->ic_bss->ni_chan && ic->ic_bss->ni_chan != IEEE80211_CHAN_ANYC &&
        ic->ic_bss->ni_chan->ic_freq) {
        cd->channel.channel = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
        cd->channel.flags = ieeeChanFlag2apple(ic->ic_bss->ni_chan->ic_flags, -1);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getSTATE(struct apple80211_state_data *sd)
{
    if (!sd)
        return kIOReturnError;

    memset(sd, 0, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    sd->state = ic->ic_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getMCS_INDEX_SET(struct apple80211_mcs_index_set_data *ad)
{
    if (!ad)
        return kIOReturnError;

    memset(ad, 0, sizeof(*ad));
    ad->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        size_t size = min(ARRAY_SIZE(ic->ic_bss->ni_rxmcs), ARRAY_SIZE(ad->mcs_set_map));
        for (int i = 0; i < size; i++)
            ad->mcs_set_map[i] = ic->ic_bss->ni_rxmcs[i];
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getVHT_MCS_INDEX_SET(struct apple80211_vht_mcs_index_set_data *data)
{
    if (!data)
        return kIOReturnError;

    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL || ic->ic_curmode < IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    data->mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getMCS_VHT(struct apple80211_mcs_vht_data *data)
{
    if (!data)
        return kIOReturnError;

    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnError;

    if (!ic || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL || ic->ic_curmode < IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    data->guard_interval = (ieee80211_node_supports_vht_sgi80(ic->ic_bss) || ieee80211_node_supports_vht_sgi160(ic->ic_bss)) ? APPLE80211_GI_SHORT : APPLE80211_GI_LONG;
    data->index = ic->ic_bss->ni_txmcs;
    data->nss = driverInfo->getTxNSS();
    switch (ic->ic_bss->ni_chw) {
        case IEEE80211_CHAN_WIDTH_40:
            data->bw = 40;
            break;
        case IEEE80211_CHAN_WIDTH_80:
            data->bw = 80;
            break;
        case IEEE80211_CHAN_WIDTH_80P80:
        case IEEE80211_CHAN_WIDTH_160:
            data->bw = 160;
            break;
            
        default:
            data->bw = 20;
            break;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getRATE_SET(struct apple80211_rate_set_data *ad)
{
    if (!ad)
        return kIOReturnError;

    memset(ad, 0, sizeof(*ad));
    ad->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        ad->num_rates = MIN((uint32_t)ic->ic_bss->ni_rates.rs_nrates, (uint32_t)ARRAY_SIZE(ad->rates));
        size_t size = min(ic->ic_bss->ni_rates.rs_nrates, ARRAY_SIZE(ad->rates));
        for (int i=0; i < size; i++) {
            ad->rates[i].version = APPLE80211_VERSION;
            ad->rates[i].rate = ic->ic_bss->ni_rates.rs_rates[i];
            ad->rates[i].flags = 0;
        }
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getOP_MODE(struct apple80211_opmode_data *od)
{
    if (!od)
        return kIOReturnError;

    memset(od, 0, sizeof(*od));
    od->version = APPLE80211_VERSION;
    od->op_mode = APPLE80211_M_STA;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getTXPOWER(struct apple80211_txpower_data *txd)
{
    if (!txd)
        return kIOReturnError;

    memset(txd, 0, sizeof(*txd));
    txd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN) {
        txd->txpower = ic->ic_txpower;
        txd->txpower_unit = APPLE80211_UNIT_PERCENT;
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getRATE(struct apple80211_rate_data *rd)
{
    if (!rd)
        return kIOReturnError;

    memset(rd, 0, sizeof(*rd));
    rd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_bss == NULL)
        return 6;
    int nss;
    int sgi;
    int index = 0;
    if (ic->ic_state == IEEE80211_S_RUN) {
        rd->num_radios = 1;
        sgi = ieee80211_node_supports_sgi(ic->ic_bss);
        if (ic->ic_curmode == IEEE80211_MODE_11AC) {
            if (sgi)
                index += 1;
            ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
            if (!driverInfo)
                return kIOReturnError;
            nss = driverInfo->getTxNSS();
            if (nss < 1 || nss > 2)
                return kIOReturnError;
            switch (ic->ic_bss->ni_chw) {
                case IEEE80211_CHAN_WIDTH_40:
                    index += 4;
                    break;
                case IEEE80211_CHAN_WIDTH_80:
                    index += 8;
                    break;
                case IEEE80211_CHAN_WIDTH_80P80:
                case IEEE80211_CHAN_WIDTH_160:
                    index += 12;
                    break;

                default:
                    break;
            }
            index += 2 * (nss - 1);
            if (index < 0 || index >= IEEE80211_VHT_NUM_RATESETS)
                return kIOReturnError;
            const struct ieee80211_vht_rateset *rs = &ieee80211_std_ratesets_11ac[index];
            if (rs->nrates == 0)
                return kIOReturnError;
            uint8_t mcs = MIN((uint8_t)ic->ic_bss->ni_txmcs, (uint8_t)(rs->nrates - 1));
            rd->rate[0] = rs->rates[mcs] / 2;
        } else if (ic->ic_curmode == IEEE80211_MODE_11N) {
            int is_40mhz = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_40;
            uint8_t txmcs = ic->ic_bss->ni_txmcs;
            if (txmcs >= IEEE80211_HT_RATESET_NUM_MCS)
                return kIOReturnError;
            if (sgi)
                index += 1;
            if (is_40mhz)
                index += (IEEE80211_HT_RATESET_MIMO4_SGI + 1);
            nss = txmcs / 8 + 1;
            index += 2 * (nss - 1);
            if (index < 0 || index >= IEEE80211_HT_RATESET_CBW40_MIMO4_SGI + 1)
                return kIOReturnError;
            rd->rate[0] = ieee80211_std_ratesets_11n[index].rates[txmcs % IEEE80211_HT_RATESET_MAX_NRATES] / 2;
        } else {
            if (ic->ic_bss->ni_rates.rs_nrates == 0 || ic->ic_bss->ni_txrate >= ic->ic_bss->ni_rates.rs_nrates)
                return kIOReturnError;
            rd->rate[0] = ic->ic_bss->ni_rates.rs_rates[ic->ic_bss->ni_txrate];
        }
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getBSSID(struct apple80211_bssid_data *bd)
{
    if (!bd)
        return kIOReturnError;

    memset(bd, 0, sizeof(*bd));
    bd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        memcpy(bd->bssid.octet, ic->ic_bss->ni_bssid, APPLE80211_ADDR_LEN);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getRSSI(struct apple80211_rssi_data *rd)
{
    if (!rd)
        return kIOReturnError;

    memset(rd, 0, sizeof(*rd));
    rd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        rd->num_radios = 1;
        rd->rssi_unit = APPLE80211_UNIT_DBM;
        rd->rssi[0] = rd->aggregate_rssi
        = rd->rssi_ext[0]
        = rd->aggregate_rssi_ext
        = -(0 - IWM_MIN_DBM - ic->ic_bss->ni_rssi);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportItlwmSkywalkInterface::
getRSN_IE(struct apple80211_rsn_ie_data *data)
{
#ifdef USE_APPLE_SUPPLICANT
    if (!data || !fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    const uint8_t *rsn_ie = NULL;
    if (ic->ic_rsn_ie_override[1] > 0) {
        rsn_ie = ic->ic_rsn_ie_override;
    } else if (ic->ic_bss && ic->ic_bss->ni_rsnie) {
        rsn_ie = ic->ic_bss->ni_rsnie;
    }
    if (!rsn_ie) {
        return kIOReturnError;
    }

    uint32_t rsn_len = 2 + rsn_ie[1];
    if (rsn_len < 2 || rsn_len > APPLE80211_MAX_RSN_IE_LEN) {
        XYLog("invalid stored RSN IE length: %u\n", rsn_len);
        return kIOReturnError;
    }

    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->len = rsn_len;
    memcpy(data->ie, rsn_ie, data->len);
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwmSkywalkInterface::
setRSN_IE(struct apple80211_rsn_ie_data *data)
{
#ifdef USE_APPLE_SUPPLICANT
    if (!data || !fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    static_assert(sizeof(ic->ic_rsn_ie_override) == APPLE80211_MAX_RSN_IE_LEN, "Max RSN IE length mismatch");

    if (data->len < 2 || data->len > APPLE80211_MAX_RSN_IE_LEN || data->len != (uint32_t)data->ie[1] + 2) {
        XYLog("invalid override RSN IE length. reported=%u parsed=%u\n", data->len, (uint32_t)data->ie[1] + 2);
        return kIOReturnBadArgument;
    }

    memset(ic->ic_rsn_ie_override, 0, sizeof(ic->ic_rsn_ie_override));
    memcpy(ic->ic_rsn_ie_override, data->ie, data->len);
    if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != nullptr)
        ieee80211_save_ie(data->ie, &ic->ic_bss->ni_rsnie);
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwmSkywalkInterface::
getAP_IE_LIST(struct apple80211_ap_ie_data *data)
{
    if (!data)
        return kIOReturnError;

    uint32_t requestedLen = data->len;
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL || ic->ic_bss->ni_rsnie_tlv == NULL || ic->ic_bss->ni_rsnie_tlv_len == 0 || ic->ic_bss->ni_rsnie_tlv_len > requestedLen || ic->ic_bss->ni_rsnie_tlv_len > sizeof(data->ie_data))
        return kIOReturnError;
    data->len = ic->ic_bss->ni_rsnie_tlv_len;
    memcpy(data->ie_data, ic->ic_bss->ni_rsnie_tlv, data->len);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getNOISE(struct apple80211_noise_data *nd)
{
    if (!nd)
        return kIOReturnError;

    memset(nd, 0, sizeof(*nd));
    nd->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnError;

    if (ic && ic->ic_state == IEEE80211_S_RUN) {
        nd->num_radios = 1;
        nd->noise[0]
        = nd->aggregate_noise = -driverInfo->getBSSNoise();
        nd->noise_unit = APPLE80211_UNIT_DBM;
        return kIOReturnSuccess;
    }
    return 6;
}

#if __IO80211_TARGET >= __MAC_15_0
static uint32_t airitlPowersaveLevel();
#endif
IOReturn AirportItlwmSkywalkInterface::
getPOWERSAVE(struct apple80211_powersave_data *pd)
{
    if (!pd)
        return kIOReturnError;

    memset(pd, 0, sizeof(*pd));
    pd->version = APPLE80211_VERSION;
#if __IO80211_TARGET >= __MAC_15_0
    // Apple getPOWERSAVE returns the stored value.
    pd->powersave_level = airitlPowersaveLevel();
#else
    pd->powersave_level = APPLE80211_POWERSAVE_MODE_DISABLED;
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getNSS(struct apple80211_nss_data *data)
{
    if (!data)
        return kIOReturnError;

    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnError;

    data->nss = driverInfo->getTxNSS();
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setASSOCIATE(struct apple80211_assoc_data *ad)
{
    if (!ad || !fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;
    if (ad->ad_ssid_len > IEEE80211_NWID_LEN) {
        XYLog("invalid association SSID length: %u\n", ad->ad_ssid_len);
        return kIOReturnBadArgument;
    }

    uint32_t safe_ssid_len = MIN(ad->ad_ssid_len, (uint32_t)APPLE80211_MAX_SSID_LEN);
    char ssid_buf[APPLE80211_MAX_SSID_LEN + 1];
    memset(ssid_buf, 0, sizeof(ssid_buf));
    if (safe_ssid_len != 0)
        memcpy(ssid_buf, ad->ad_ssid, safe_ssid_len);

    XYLog("%s [%s] mode=%d ad_auth_lower=%d ad_auth_upper=%d rsn_ie_len=%d%s%s%s%s%s%s%s\n", __FUNCTION__, ssid_buf, ad->ad_mode, ad->ad_auth_lower, ad->ad_auth_upper, ad->ad_rsn_ie_len,
          (ad->ad_flags & 2) ? ", Instant Hotspot" : "",
          (ad->ad_flags & 4) ? ", Auto Instant Hotspot" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 1) ? ", don't disassociate" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 2) ? ", don't blacklist" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 4) ? ", closed Network" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 8) ? ", 802.1X" : "",
          (ad->ad_rsn_ie[APPLE80211_MAX_RSN_IE_LEN] & 0x20) ? ", force BSSID" : "");
    
    struct apple80211_rsn_ie_data rsn_ie_data;
    struct apple80211_authtype_data auth_type_data;
    
    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;

    if (ad->ad_mode != APPLE80211_AP_MODE_IBSS) {
        disassocIsVoluntary = false;
        auth_type_data.version = APPLE80211_VERSION;
        auth_type_data.authtype_upper = ad->ad_auth_upper;
        auth_type_data.authtype_lower = ad->ad_auth_lower;
        setAUTH_TYPE(&auth_type_data);
        if (ad->ad_rsn_ie_len != 0) {
            memset(&rsn_ie_data, 0, sizeof(rsn_ie_data));
            rsn_ie_data.version = APPLE80211_VERSION;
            if (ad->ad_rsn_ie_len < 2) {
                XYLog("invalid short RSN IE length: %u\n", ad->ad_rsn_ie_len);
                return kIOReturnBadArgument;
            }
            rsn_ie_data.len = ad->ad_rsn_ie[1] + 2;
            if (ad->ad_rsn_ie_len > APPLE80211_MAX_RSN_IE_LEN ||
                rsn_ie_data.len > APPLE80211_MAX_RSN_IE_LEN ||
                rsn_ie_data.len > ad->ad_rsn_ie_len) {
                XYLog("invalid RSN IE length. reported=%u parsed=%u\n", ad->ad_rsn_ie_len, rsn_ie_data.len);
                return kIOReturnBadArgument;
            }
            memcpy(rsn_ie_data.ie, ad->ad_rsn_ie, rsn_ie_data.len);
            IOReturn rsnResult = setRSN_IE(&rsn_ie_data);
            if (rsnResult != kIOReturnSuccess)
                return rsnResult;
        }

        IOReturn associateResult = associateSSID(ad->ad_ssid, ad->ad_ssid_len, ad->ad_bssid, ad->ad_auth_lower, ad->ad_auth_upper, ad->ad_key.key, ad->ad_key.key_len, ad->ad_key.key_index);
        if (associateResult != kIOReturnSuccess)
            return associateResult;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setDISASSOCIATE(struct apple80211_disassoc_data *ad)
{
    XYLog("%s\n", __FUNCTION__);
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
    
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;
    
    disassocIsVoluntary = true;

    ieee80211_del_ess(ic, nullptr, 0, 1);
    ieee80211_deselect_ess(ic);
#ifdef USE_APPLE_SUPPLICANT
    ic->ic_rsn_ie_override[1] = 0;
#endif
    ic->ic_assoc_status = APPLE80211_STATUS_UNAVAILABLE;
    ic->ic_deauth_reason = APPLE80211_REASON_ASSOC_LEAVING;
    ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getSUPPORTED_CHANNELS(struct apple80211_sup_channel_data *ad)
{
    if (!ad)
        return kIOReturnError;
    memset(ad, 0, sizeof(*ad));
    ad->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;
    for (int i = 0; i < IEEE80211_CHAN_MAX; i++) {
        if (ad->num_channels >= ARRAY_SIZE(ad->supported_channels))
            break;
        if (ic->ic_channels[i].ic_freq != 0) {
            ad->supported_channels[ad->num_channels].channel = ieee80211_chan2ieee(ic, &ic->ic_channels[i]);
            ad->supported_channels[ad->num_channels].flags = ieeeChanFlag2appleScanFlagVentura(ic->ic_channels[i].ic_flags);
            ad->num_channels++;
        }
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getLOCALE(struct apple80211_locale_data *ld)
{
    if (!ld)
        return kIOReturnError;
    memset(ld, 0, sizeof(*ld));
    ld->version = APPLE80211_VERSION;
    ld->locale  = APPLE80211_LOCALE_FCC;
    
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getDEAUTH(struct apple80211_deauth_data *da)
{
    if (!da)
        return kIOReturnError;
    memset(da, 0, sizeof(*da));
    da->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;
    da->deauth_reason = ic->ic_deauth_reason;
//    XYLog("%s, %d\n", __FUNCTION__, da->deauth_reason);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getASSOCIATION_STATUS(struct apple80211_assoc_status_data *hv)
{
    if (!hv)
        return kIOReturnError;

    memset(hv, 0, sizeof(*hv));
    hv->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    if (ic->ic_state == IEEE80211_S_RUN)
        hv->status = APPLE80211_STATUS_SUCCESS;
    else
        hv->status = APPLE80211_STATUS_UNAVAILABLE;
//    XYLog("%s, %d\n", __FUNCTION__, hv->status);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setSCANCACHE_CLEAR(void *req)
{
    XYLog("%s\n", __FUNCTION__);
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;
    //if doing background or active scan, don't free nodes.
    if ((ic->ic_flags & IEEE80211_F_BGSCAN) || (ic->ic_flags & IEEE80211_F_ASCAN))
        return kIOReturnSuccess;
    fNextNodeToSend = NULL;
    fScanResultWrapping = false;
    ieee80211_free_allnodes(ic, 0);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
setDEAUTH(struct apple80211_deauth_data *da)
{
    if (!da)
        return kIOReturnBadArgument;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    AIRITL_DBG("%s reason=%u\n", __FUNCTION__, da->deauth_reason);
    return setDISASSOCIATE(nullptr);
}

IOReturn AirportItlwmSkywalkInterface::
getMCS(struct apple80211_mcs_data* md)
{
    if (!md)
        return kIOReturnError;

    memset(md, 0, sizeof(*md));
    md->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN ||  ic->ic_bss == NULL)
        return 6;
    md->index = ic->ic_bss->ni_txmcs;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getLINK_CHANGED_EVENT_DATA(struct apple80211_link_changed_event_data *ed)
{
    if (ed == nullptr)
        return 16;
    bzero(ed, sizeof(apple80211_link_changed_event_data));
    if (!fHalService || !instance)
        return kIOReturnError;
    
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;
    
    ed->isLinkDown = !(instance->currentStatus & kIONetworkLinkActive);
    if (ed->isLinkDown) {
        ed->voluntary = disassocIsVoluntary;
        ed->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
    } else if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss)
        ed->rssi = -(0 - IWM_MIN_DBM - ic->ic_bss->ni_rssi);
    else
        return kIOReturnError;
    XYLog("Link %s, reason: %d, voluntary: %d\n", ed->isLinkDown ? "down" : "up", ed->reason, ed->voluntary);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::beginScanRequest(bool cacheOnly)
{
    if (!fHalService || !scanSource)
        return kIOReturnNotAttached;
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state <= IEEE80211_S_INIT)
        return kIOReturnNotReady;
    // In native mode WCL owns scanning and the HAL starts none; a legacy
    // foreground request would wait for a SCAN_DONE that never comes.
    if (ic->ic_native_controlled)
        cacheOnly = true;
    const UInt32 pendingState = cacheOnly ? 2 : 1;
    if (!OSCompareAndSwap(0, pendingState, &fScanRequestPending))
        return kIOReturnBusy;

    scanSource->cancelTimeout();
    fNextNodeToSend = NULL;
    fScanResultWrapping = false;
    if (cacheOnly) {
        // This request uses the existing cache. No radio scan was started.
        scanSource->setTimeoutMS(0);
        scanSource->enable();
        return kIOReturnSuccess;
    }

    // An existing foreground scan can satisfy this request. A connected station
    // starts a cache scan only when net80211 permits it.
    if (ic->ic_state != IEEE80211_S_SCAN &&
        !(ic->ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)))
        ieee80211_begin_cache_bgscan(&ic->ic_ac.ac_if);
    if (ic->ic_state == IEEE80211_S_SCAN ||
        (ic->ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)))
        return kIOReturnSuccess;

    // The HAL did not start a scan. Do not publish a successful completion.
    OSCompareAndSwap(1, 0, &fScanRequestPending);
    return kIOReturnBusy;
}

void AirportItlwmSkywalkInterface::completeScanRequest(bool cacheOnly)
{
    const UInt32 pendingState = cacheOnly ? 2 : 1;
    if (!OSCompareAndSwap(pendingState, 0, &fScanRequestPending))
        return;
    UInt32 status = 0;
    postMessage(APPLE80211_M_SCAN_DONE, &status, sizeof(status), true);
}

IOReturn AirportItlwmSkywalkInterface::
setSCAN_REQ(struct apple80211_scan_data *sd)
{
    if (!sd)
        return kIOReturnBadArgument;
    return beginScanRequest(sd->scan_type == APPLE80211_SCAN_TYPE_FAST ||
                            sd->scan_type == APPLE80211_SCAN_TYPE_PASSIVE);
}

IOReturn AirportItlwmSkywalkInterface::
setSCAN_REQ_MULTIPLE(struct apple80211_scan_multiple_data *sd)
{
    if (!sd || sd->ssid_count > ARRAY_SIZE(sd->ssids) ||
        sd->bssid_count > ARRAY_SIZE(sd->bssids) ||
        sd->num_channels > ARRAY_SIZE(sd->channels))
        return kIOReturnBadArgument;
    return beginScanRequest(sd->scan_type == APPLE80211_SCAN_TYPE_FAST ||
                            sd->scan_type == APPLE80211_SCAN_TYPE_PASSIVE);
}

extern OSDictionary *convertScanToDictionary(apple80211_scan_result *a1);

static int convertNodeToScanResult(ItlHalService *fHalService, struct ieee80211_node *fNextNodeToSend, apple80211_scan_result *result)
{
    if (!fHalService || !fNextNodeToSend || !result || !fNextNodeToSend->ni_chan)
        return EINVAL;

    struct ieee80211com *ic = fHalService->get80211Controller();
    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!ic || !driverInfo)
        return EINVAL;

    bzero(result, sizeof(*result));
    result->version = APPLE80211_VERSION;
    if (fNextNodeToSend->ni_rsnie_tlv && fNextNodeToSend->ni_rsnie_tlv_len > 0) {
        size_t ie_len = MIN((size_t)fNextNodeToSend->ni_rsnie_tlv_len, sizeof(result->asr_ie_data));
        result->asr_ie_len = (int16_t)ie_len;
        memcpy(result->asr_ie_data, fNextNodeToSend->ni_rsnie_tlv, ie_len);
    } else {
        result->asr_ie_len = 0;
    }
    result->asr_beacon_int = fNextNodeToSend->ni_intval;
    result->asr_nrates = MIN((uint8_t)fNextNodeToSend->ni_rates.rs_nrates, (uint8_t)ARRAY_SIZE(result->asr_rates));
    for (int i = 0; i < result->asr_nrates; i++ )
        result->asr_rates[i] = fNextNodeToSend->ni_rates.rs_rates[i];
    result->asr_age = (uint32_t)(airport_up_time() - fNextNodeToSend->ni_age_ts);
    result->asr_cap = fNextNodeToSend->ni_capinfo;
    result->asr_channel.version = APPLE80211_VERSION;
    result->asr_channel.channel = ieee80211_chan2ieee(ic, fNextNodeToSend->ni_chan);
    result->asr_channel.flags = ieeeChanFlag2appleScanFlagVentura(fNextNodeToSend->ni_chan->ic_flags);
    result->asr_noise = -driverInfo->getBSSNoise();
    result->asr_rssi = -(0 - IWM_MIN_DBM - fNextNodeToSend->ni_rssi);
    memcpy(result->asr_bssid, fNextNodeToSend->ni_bssid, IEEE80211_ADDR_LEN);
    result->asr_ssid_len = MIN((uint8_t)fNextNodeToSend->ni_esslen, (uint8_t)sizeof(result->asr_ssid));
    if (result->asr_ssid_len != 0)
        memcpy(&result->asr_ssid, fNextNodeToSend->ni_essid, result->asr_ssid_len);
    return 0;
}

IOReturn AirportItlwmSkywalkInterface::
getCURRENT_NETWORK(apple80211_scan_result *sr)
{
    if (!sr)
        return kIOReturnError;

    bzero(sr, sizeof(*sr));
    sr->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL)
        return kIOReturnError;
    if (convertNodeToScanResult(fHalService, ic->ic_bss, sr) != 0)
        return kIOReturnError;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getCOLOCATED_NETWORK_SCOPE_ID(apple80211_colocated_network_scope_id *as)
{
    if (!as)
        return kIOReturnBadArgument;
    memset(as, 0, sizeof(*as));
    as->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::
getSCAN_RESULT(struct apple80211_scan_result *sr)
{
    if (!sr)
        return kIOReturnError;

    bzero(sr, sizeof(*sr));
    sr->version = APPLE80211_VERSION;
    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    if (fNextNodeToSend == NULL) {
        if (fScanResultWrapping) {
            fScanResultWrapping = false;
            return 5;
        } else {
            fNextNodeToSend = RB_MIN(ieee80211_tree, &ic->ic_tree);
            if (fNextNodeToSend == NULL) {
                return 5;
            }
        }
    }
//    XYLog("%s ni_bssid=%s ni_essid=%s channel=%d flags=%d asr_cap=%d asr_nrates=%d asr_ssid_len=%d asr_ie_len=%d asr_rssi=%d\n", __FUNCTION__, ether_sprintf(fNextNodeToSend->ni_bssid), fNextNodeToSend->ni_essid, ieee80211_chan2ieee(ic, fNextNodeToSend->ni_chan), ieeeChanFlag2apple(fNextNodeToSend->ni_chan->ic_flags, -1), fNextNodeToSend->ni_capinfo, fNextNodeToSend->ni_rates.rs_nrates, fNextNodeToSend->ni_esslen, fNextNodeToSend->ni_rsnie_tlv == NULL ? 0 : fNextNodeToSend->ni_rsnie_tlv_len, fNextNodeToSend->ni_rssi);
    if (convertNodeToScanResult(fHalService, fNextNodeToSend, sr) != 0) {
        fNextNodeToSend = NULL;
        return kIOReturnError;
    }
    
    fNextNodeToSend = RB_NEXT(ieee80211_tree, &ic->ic_tree, fNextNodeToSend);
    if (fNextNodeToSend == NULL)
        fScanResultWrapping = true;

    return kIOReturnSuccess;
}

#include "AiritlNativeWcl.inc"
#include "AiritlNativeConfig.inc"
#include "AiritlNativeR21.inc"
#include "AiritlNativeR23.inc"
#include "AiritlNativeRegulatory.inc"
#include "AiritlNativeStockControl.inc"
#include "AiritlNativeBss.inc"
#include "AiritlNativeRoam.inc"
