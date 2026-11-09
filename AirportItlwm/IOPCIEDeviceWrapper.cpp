//
//  IOPCIEDeviceWrapper.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "IOPCIEDeviceWrapper.hpp"
#include "Apple80211.h"
#include <libkern/OSAtomic.h>

#include "ItlIwm.hpp"
#include "ItlIwx.hpp"
#include "ItlIwn.hpp"
#include <HAL/AiritlDebug.h>

#define super IOService
OSDefineMetaClassAndStructors(IOPCIEDeviceWrapper, IOService);

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static IOPMPowerState powerStateArray[2] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    UInt16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    UInt16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

extern IO80211WorkQueue *_fWorkloop;

IOWorkLoop *IOPCIEDeviceWrapper::getWorkLoop() const
{
    return fOwnedWorkQueue;
}

static bool isAirportItlwmNetworkControllerPersonality(OSDictionary *table)
{
    if (!table)
        return false;

    OSString *bundleIdentifier = OSDynamicCast(OSString, table->getObject("CFBundleIdentifier"));
    OSString *ioClass = OSDynamicCast(OSString, table->getObject("IOClass"));
    if (!bundleIdentifier || !ioClass ||
        !bundleIdentifier->isEqualTo("com.zxystd.AirportItlwm") ||
        !ioClass->isEqualTo("AirportItlwm"))
        return false;

    OSDictionary *propertyMatch = OSDynamicCast(OSDictionary, table->getObject("IOPropertyMatch"));
    if (!propertyMatch)
        return false;

    return propertyMatch->getObject("AirportItlwmControllerMatch") != NULL;
}

// NOTE: do NOT override the 1-arg matchPropertyTable(OSDictionary*). IOService's default
// 2-arg matchPropertyTable(table, score) calls the virtual 1-arg; if the 1-arg forwards back
// to the 2-arg (as it did), super::matchPropertyTable -> virtual 1-arg -> our 1-arg -> our 2-arg
// recurses infinitely -> kernel stack overflow -> double fault -> reboot. Only the 2-arg is needed.
bool IOPCIEDeviceWrapper::
matchPropertyTable(OSDictionary *table, SInt32 *score)
{
    bool controllerPersonality = isAirportItlwmNetworkControllerPersonality(table);
    bool matched = super::matchPropertyTable(table, score);
    if (matched) {
        if (controllerPersonality) {
            AIRITL_DBG("%s::matchPropertyTable AirportItlwm controller personality standard-match pci=%p property=%p score=%d\n",
                  getName(), pciNub, getProperty("AirportItlwmControllerMatch"), score ? *score : 0);
        }
        return true;
    }

    if (!controllerPersonality)
        return false;

    bool ready = pciNub && getProperty("AirportItlwmControllerMatch");
    AIRITL_DBG("%s::matchPropertyTable AirportItlwm controller personality fallback-match ready=%d pci=%p property=%p score=%d\n",
          getName(), ready, pciNub, getProperty("AirportItlwmControllerMatch"), score ? *score : 0);
    if (!ready)
        return false;

    if (score && *score < 70000)
        *score = 70000;
    return true;
}

IOService* IOPCIEDeviceWrapper::
probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool isMatch = false;
    super::probe(provider, score);
    UInt8 msiCap = 0;
    UInt8 msixCap = 0;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device)
        return NULL;
    if (ItlIwx::iwx_match(device)) {
        isMatch = true;
        AIRITL_DBG("IOPCIEDeviceWrapper::probe matched ItlIwx provider=%p score=%d\n", provider, score ? *score : 0);
    }
    if (!isMatch && ItlIwm::iwm_match(device)) {
        isMatch = true;
        AIRITL_DBG("IOPCIEDeviceWrapper::probe matched ItlIwm provider=%p score=%d\n", provider, score ? *score : 0);
    }
    if (!isMatch && ItlIwn::iwn_match(device)) {
        isMatch = true;
        AIRITL_DBG("IOPCIEDeviceWrapper::probe matched ItlIwn provider=%p score=%d\n", provider, score ? *score : 0);
    }
    if (isMatch) {
        XYLog("%s Found\n", __FUNCTION__);
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
        if (msixCap)
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
        if (msiCap)
            pciMsiSetEnable(device, msiCap, 1);
        if (!msiCap && !msixCap) {
            XYLog("%s No MSI cap\n", __FUNCTION__);
            IOLog("IOPCIEDeviceWrapper::probe rejected matched PCI device without MSI/MSI-X provider=%p\n", provider);
            return NULL;
        }
        this->pciNub = device;
        AIRITL_DBG("IOPCIEDeviceWrapper::probe accepted PCI provider=%p msiCap=0x%x msixCap=0x%x\n",
              provider, msiCap, msixCap);
        return this;
    }
    AIRITL_DBG("IOPCIEDeviceWrapper::probe no supported Intel Wi-Fi match for provider=%p\n", provider);
    return NULL;
}

// Probe candidates must not share an initialized HAL. A rejected candidate
// releases its own object; later candidates get fresh controller/lock state.
ItlHalService *IOPCIEDeviceWrapper::createHalService() const
{
    if (!pciNub || !getProperty("AirportItlwmControllerMatch"))
        return NULL;
    if (ItlIwx::iwx_match(pciNub))
        return new ItlIwx;
    if (ItlIwm::iwm_match(pciNub))
        return new ItlIwm;
    if (ItlIwn::iwn_match(pciNub))
        return new ItlIwn;
    return NULL;
}

bool IOPCIEDeviceWrapper::
start(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fOwnedWorkQueue)
        return false;
    fOwnedWorkQueue = IO80211WorkQueue::workQueue();
    if (!fOwnedWorkQueue)
        return false;
    // The global is a borrowed alias for the single active wrapper.
    if (!OSCompareAndSwapPtr(NULL, fOwnedWorkQueue, &_fWorkloop)) {
        OSSafeReleaseNULL(fOwnedWorkQueue);
        return false;
    }
    if (!super::start(provider)) {
        OSCompareAndSwapPtr(fOwnedWorkQueue, NULL, &_fWorkloop);
        OSSafeReleaseNULL(fOwnedWorkQueue);
        return false;
    }
    IOLog("%s::super start succeed\n", getName());
    UInt8 builtIn = 0;
    OSData *builtInData = OSData::withBytes(&builtIn, sizeof(builtIn));
    if (builtInData) {
        setProperty("built-in", builtInData);
        builtInData->release();
    }
    setProperty("AirportItlwmControllerMatch", kOSBooleanTrue);
    PMinit();
    registerPowerDriver(this, powerStateArray, 2);
    provider->joinPMtree(this);
    AIRITL_DBG("%s::registering service for AirportItlwm controller match pci=%p property=%p mode=async\n",
          getName(), pciNub, getProperty("AirportItlwmControllerMatch"));
    registerService();
    return true;
}

void IOPCIEDeviceWrapper::
stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    removeProperty("AirportItlwmControllerMatch");
    PMstop();
    super::stop(provider);
}

void IOPCIEDeviceWrapper::free()
{
    if (fOwnedWorkQueue) {
        OSCompareAndSwapPtr(fOwnedWorkQueue, NULL, &_fWorkloop);
        OSSafeReleaseNULL(fOwnedWorkQueue);
    }
    super::free();
}

IOReturn IOPCIEDeviceWrapper::
setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}
