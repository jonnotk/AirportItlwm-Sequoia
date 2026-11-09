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

#ifndef ItlHalService_hpp
#define ItlHalService_hpp

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include "ItlDriverInfo.hpp"
#include "ItlDriverController.hpp"
#include "ItlNativeScanProfile.hpp"
#include "ItlNativeCurrentRates.hpp"

#include <net80211/ieee80211_var.h>

class ItlHalService : public OSObject {
    OSDeclareAbstractStructors(ItlHalService)
    
public:
    
    virtual bool attach(IOPCIDevice *device) = 0;
    
    virtual void detach(IOPCIDevice *device) = 0;
    virtual bool quiesceForDetach() = 0;
    
    virtual IOReturn enable(IONetworkInterface *interface) = 0;
    
    virtual IOReturn disable(IONetworkInterface *interface) = 0;
    
    virtual struct ieee80211com *get80211Controller() = 0;
    
    virtual ItlDriverInfo *getDriverInfo() = 0;
    
    virtual ItlDriverController *getDriverController() = 0;
    
    // Call on the main workloop gate. The HAL copies the profile before return.
    // A failed start has no callback. Cancel completes when firmware stops or
    // disable/stop quiesces the device; it never claims successful scan results.
    virtual bool supportsNativeScan() const { return false; }
    // Permanent NVM address, separate from the current private interface MAC.
    // The caller holds the main gate and validates the copied address.
    virtual bool copyPermanentHardwareAddress(uint8_t *) const { return false; }
    virtual IOReturn startNativeScan(const ItlNativeScanProfile &,
                                    ItlNativeScanCompletion, void *)
        { return kIOReturnUnsupported; }
    virtual IOReturn cancelNativeScan(uint64_t) { return kIOReturnUnsupported; }
    // Main gate only, may sleep (releases the gate). Tear the station down in
    // firmware (STA, binding, MAC context) and leave net80211 idle in SCAN with
    // the radio, firmware and regulatory state kept. Apple's AppleBCMWLANCore
    // leave/join-abort send one WLC_DISASSOC; they reset firmware only when it
    // fails (recoverFromDisassocFailure). Non-success means: use a full stop.
    virtual IOReturn nativeSoftLeave(uint64_t) { return kIOReturnUnsupported; }
    // Main gate only. Caller identity must remain live across each sleep.
    virtual IOReturn nativeWaitUntilReady(uint64_t, const uint64_t *, uint64_t)
        { return kIOReturnUnsupported; }
    // Main gate only, no wait: scan polling must not sleep in an init callback.
    virtual bool nativeScanInitializationReady() const { return false; }
    virtual bool nativeRadioStopPending() const { return false; }
    virtual bool nativeRegulatoryReady() const { return true; }
    virtual bool nativeKeyInstallPending() const { return false; }
    virtual bool nativeDataOutputReady() const { return false; }
    virtual bool nativeReassociationReady() { return false; }
    virtual bool copyNativeCurrentRates(ItlNativeCurrentRates *) { return false; }
    virtual IOReturn nativeSetHardwareAddress(const uint8_t *)
        { return kIOReturnUnsupported; }

    virtual void free() override;
    bool beginTeardown();
    bool isTearingDown() const { return teardownStarted; }
    bool hasInterruptSourceLease() const { return interruptSourceLease; }
    bool teardownMustRetainResources() const { return interruptSourceLease || teardownQuarantined; }

public:
    virtual bool initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate);
    
protected:
    
    // Call after closing device work admission, before freeing callback state.
    static IOReturn quiesceGated(OSObject *, void *, void *, void *, void *);
    void quarantineTeardown();
    void teardownStartedBeforeBinding() { teardownStarted = true; }
    bool deviceAttachComplete = false;
    bool detachQuiesced = false;
    bool detachComplete = false;
    void acquireInterruptSourceLease();
    bool releaseInterruptSource(IOInterruptEventSource *&source);

    int tsleep_nsec(void *ident, int priority, const char *wmesg, int timo);
    
    void wakeupOn(void* ident);
    
    IOEthernetController *getController();
    
    IOCommandGate *getMainCommandGate();
    
    IOWorkLoop *getMainWorkLoop();
    
private:
    bool interruptSourceLease = false;
    bool teardownStarted = false;
    bool teardownQuarantined = false;
    bool teardownQuarantineLease = false;
    IOEthernetController *controller = NULL;
    IOCommandGate *mainCommandGate = NULL;
    IOWorkLoop *mainWorkLoop = NULL;

    lck_grp_t *inner_gp = NULL;
    lck_grp_attr_t *inner_gp_attr = NULL;
    lck_attr_t *inner_attr = NULL;
    lck_mtx_t *inner_lock = NULL;
};

#endif /* ItlHalService_hpp */
