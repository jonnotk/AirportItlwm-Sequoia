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

#include "ItlIwm.hpp"

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlIwm, ItlHalService)

bool ItlIwm::quiesceForDetach()
{
    if (detachQuiesced) {
        if (taskq_is_current(systq) || taskq_is_current(com.sc_nswq)) {
            quarantineTeardown();
            return false;
        }
        return true;
    }
    if (!getMainWorkLoop() || !getMainCommandGate()) {
        teardownStartedBeforeBinding();
        bool pristine = !com.sc_st && !com.sc_ih && !deviceAttachComplete;
        if (!pristine)
            quarantineTeardown();
        return pristine;
    }
    if (!getMainWorkLoop()->inGate())
        return getMainCommandGate()->runAction(quiesceGated, static_cast<ItlHalService *>(this)) == kIOReturnSuccess;

    bool timerAdmissionClosed = beginTeardown();
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    struct iwm_softc *sc = &com;

    struct task *tasks[] = {&com.init_task, &com.newstate_task, &com.ba_task,
        &com.mac_ctxt_task, &com.chan_ctxt_task};
    // Keep IRQ delivery and all firmware state alive while active tasks return.
    for (unsigned i = 0; i < nitems(tasks); ++i)
        taskq_suspend_task(i == 1 ? com.sc_nswq : systq, tasks[i]);
    // The task barrier excludes its caller, but final queue destruction cannot.
    // A self-worker cleanup must leave its callback dependencies retained.
    if (taskq_is_current(systq) || taskq_is_current(com.sc_nswq)) {
        quarantineTeardown();
        return false;
    }
    if (!timerAdmissionClosed) {
        IOLog("itlwm: timer admission publication failed; retaining device resources\n");
        return false;
    }
    if (getMainCommandGate()->runAction(CTimeout::quiesceControllerTimers) != kIOReturnSuccess) {
        quarantineTeardown();
        return false;
    }
    IOCommandGate *gate = getMainWorkLoop()->inGate() ? getMainCommandGate() : NULL;
    for (unsigned i = 0; i < nitems(tasks); ++i)
        taskq_del_barrier_gated(i == 1 ? com.sc_nswq : systq, tasks[i], gate);
    // Only a successful attach still owns the complete DMA ring set.
    if (deviceAttachComplete && com.sc_st)
        iwm_stop_device(sc);
    deviceAttachComplete = false;
    if (!releaseInterruptSource(com.sc_ih))
        return false;
    detachQuiesced = true;
    return true;
}

void ItlIwm::
detach(IOPCIDevice *device)
{
    if (detachComplete || !quiesceForDetach())
        return;
    if (!getMainWorkLoop() || !getMainCommandGate()) {
        releaseAll();
        detachComplete = true;
        return;
    }
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    struct iwm_softc *sc = &com;


    
    for (int txq_i = 0; txq_i < nitems(sc->txq); txq_i++)
        iwm_free_tx_ring(sc, &sc->txq[txq_i]);
    iwm_rs_free(sc);
    iwm_free_rx_ring(sc, &sc->rxq);
    iwm_dma_contig_free(&sc->ict_dma);
    iwm_dma_contig_free(&sc->kw_dma);
    iwm_dma_contig_free(&sc->sched_dma);
    iwm_dma_contig_free(&sc->fw_dma);
    ieee80211_ifdetach(ifp);
    timeout_del(&com.sc_calib_to);
    timeout_free(&com.sc_calib_to);
    timeout_del(&com.sc_led_blink_to);
    timeout_free(&com.sc_led_blink_to);
    for (unsigned i = 0; i < nitems(com.sc_rxba_data); ++i) {
        timeout_free(&com.sc_rxba_data[i].session_timer);
        timeout_free(&com.sc_rxba_data[i].reorder_buf.reorder_timer);
    }
    taskq_destroy(systq);
    taskq_destroy(com.sc_nswq);
    com.sc_nswq = NULL;
    releaseAll();
    detachComplete = true;
}

bool ItlIwm::
attach(IOPCIDevice *device)
{
    pci.pa_tag = device;
    pci.workloop = getMainWorkLoop();
    if (!iwm_attach(&com, &pci)) {
        detach(device);
        releaseAll();
        return false;
    }
    deviceAttachComplete = true;
    return true;
}

void ItlIwm::
free()
{
    XYLog("%s\n", __FUNCTION__);
    releaseAll();
    super::free();
}

void ItlIwm::
releaseAll()
{
    if (isTearingDown() && teardownMustRetainResources())
        return;
    if (!releaseInterruptSource(com.sc_ih))
        return;
    XYLog("%s\n", __FUNCTION__);
    pci_intr_handle *intrHandler = com.ih;
    if (com.sc_calib_to) {
        timeout_del(&com.sc_calib_to);
        timeout_free(&com.sc_calib_to);
    }
    if (com.sc_led_blink_to) {
        timeout_del(&com.sc_led_blink_to);
        timeout_free(&com.sc_led_blink_to);
    }
    if (intrHandler) {
        if (intrHandler->intr && intrHandler->workloop) {
//            intrHandler->intr->disable();
            intrHandler->workloop->removeEventSource(intrHandler->intr);
            intrHandler->intr->release();
        }
        intrHandler->intr = NULL;
        intrHandler->workloop = NULL;
        intrHandler->arg = NULL;
        intrHandler->dev = NULL;
        intrHandler->func = NULL;
        intrHandler->release();
        com.ih = NULL;
    }
    // Callback and task users have drained before their BAR mapping goes away.
    OSSafeReleaseNULL(com.sc_st);
    com.sc_sh = NULL;
    com.sc_sz = 0;
    pci.pa_tag = NULL;
    pci.workloop = NULL;
}

IOReturn ItlIwm::
enable(IONetworkInterface *netif)
{
    if (isTearingDown() || !getMainWorkLoop() || !deviceAttachComplete)
        return kIOReturnNotReady;
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    if (ifp->if_flags & IFF_UP) {
        XYLog("%s already in activating state\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    ifp->if_flags |= IFF_UP;
    iwm_activate(&com, DVACT_RESUME);
    iwm_activate(&com, DVACT_WAKEUP);
    return kIOReturnSuccess;
}

IOReturn ItlIwm::
disable(IONetworkInterface *netif)
{
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    if (!(ifp->if_flags & IFF_UP)) {
        XYLog("%s already in diactivating state\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    ifp->if_flags &= ~IFF_UP;
    iwm_activate(&com, DVACT_QUIESCE);
    return kIOReturnSuccess;
}

struct ieee80211com *ItlIwm::
get80211Controller()
{
    return &com.sc_ic;
}

ItlDriverInfo *ItlIwm::
getDriverInfo()
{
    return this;
}

ItlDriverController *ItlIwm::
getDriverController()
{
    return this;
}

void ItlIwm::
clearScanningFlags()
{
    com.sc_flags &= ~(IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN);
}

IOReturn ItlIwm::
setMulticastList(IOEthernetAddress *addr, int count)
{
    struct ieee80211com *ic = &com.sc_ic;
    struct iwm_mcast_filter_cmd *cmd;
    int len;
    uint8_t addr_count;
    int err;
    
    if (ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL)
        return kIOReturnError;
    addr_count = count;
    if (count > IWM_MAX_MCAST_FILTERING_ADDRESSES)
        addr_count = 0;
    if (addr == NULL)
        addr_count = 0;
    len = roundup(sizeof(struct iwm_mcast_filter_cmd) + addr_count * ETHER_ADDR_LEN, 4);
    XYLog("%s multicast count=%d bssid=%s\n", __FUNCTION__, count, ether_sprintf(ic->ic_bss->ni_bssid));
    cmd = (struct iwm_mcast_filter_cmd *)malloc(len, 0, 0);
    if (!cmd)
        return kIOReturnError;
    cmd->pass_all = addr_count == 0;
    cmd->count = addr_count;
    cmd->port_id = 0;
    IEEE80211_ADDR_COPY(cmd->bssid, ic->ic_bss->ni_bssid);
    if (addr_count > 0)
        memcpy(cmd->addr_list,
               addr->bytes, ETHER_ADDR_LEN * cmd->count);
    err = iwm_send_cmd_pdu(&com, IWM_MCAST_FILTER_CMD, IWM_CMD_ASYNC, len,
                     cmd);
    ::free(cmd);
    return err ? kIOReturnError : kIOReturnSuccess;
}

const char *ItlIwm::
getFirmwareVersion()
{
    return com.sc_fwver;
}

const char *ItlIwm::
getFirmwareName()
{
    return com.sc_fwname;
}

UInt32 ItlIwm::
supportedFeatures()
{
    return kIONetworkFeatureMultiPages;
}

const char *ItlIwm::
getFirmwareCountryCode()
{
    return com.sc_fw_mcc;
}

uint32_t ItlIwm::
getTxQueueSize()
{
    return IWM_TX_RING_COUNT;
}

int16_t ItlIwm::
getBSSNoise()
{
    return com.sc_noise;
}

bool ItlIwm::
is5GBandSupport()
{
    return com.sc_nvm.sku_cap_band_52GHz_enable;
}

int ItlIwm::
getTxNSS()
{
    return iwm_mimo_enabled(&com) &&
    (com.sc_ic.ic_bss != NULL && com.sc_ic.ic_bss->ni_rx_nss > 1) ?
    2 : 1;
}
