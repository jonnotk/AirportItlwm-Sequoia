//
//  CTimeout.cpp
//  AppleIntelWifiAdapter
//
//  Created by 钟先耀 on 2020/1/30.
//  Copyright © 2020 钟先耀. All rights reserved.
//

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

#include <sys/CTimeout.hpp>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSCollectionIterator.h>

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
#include <IOKit/IOWorkLoop.h>
#endif

extern IOCommandGate *_fCommandGate;

OSDefineMetaClassAndStructors(CTimeoutRegistry, OSObject)

bool CTimeoutRegistry::init()
{
    if (!OSObject::init())
        return false;
    timers = OSSet::withCapacity(8);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if (!timers)
        return false;
    drainIterator = OSCollectionIterator::withCollection(timers);
    if (!drainIterator)
        return false;
    drainIterator->reset();
    return drainIterator->isValid();
#else
    return timers != NULL;
#endif
}

bool CTimeout::init()
{
    return OSObject::init();
}

void CTimeoutRegistry::free()
{
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    OSSafeReleaseNULL(drainIterator);
#endif
    OSSafeReleaseNULL(timers);
    OSObject::free();
}

bool CTimeout::initializeControllerTimers(IOService *controller)
{
    OSObject *existing = controller->getProperty("ItlwmCTimers");
    if (existing) {
        CTimeoutRegistry *holder = OSDynamicCast(CTimeoutRegistry, existing);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
        if (!holder || !holder->timers || holder->drainBusy || !holder->drainIterator ||
            holder->timers->getCount() != 0)
            return false;
        holder->drainIterator->reset();
        return holder->drainIterator->isValid();
#else
        return holder && holder->timers && holder->timers->getCount() == 0;
#endif
    }
    CTimeoutRegistry *holder = new CTimeoutRegistry;
    if (!holder)
        return false;
    if (!holder->init()) {
        holder->release();
        return false;
    }
    bool result = controller->setProperty("ItlwmCTimers", holder);
    holder->release();
    return result;
}

IOReturn CTimeout::quiesceControllerTimers(OSObject *target, void *, void *, void *, void *)
{
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)

    IOService *controller = OSDynamicCast(IOService, target);
    if (!controller || controller->getProperty("ItlwmTeardown") != kOSBooleanTrue)
        return kIOReturnNotReady;
    IOCommandGate *gate = _fCommandGate;
    IOWorkLoop *loop = gate ? gate->getWorkLoop() : NULL;
    if (!gate || !loop || !loop->inGate())
        return kIOReturnNotReady;
    CTimeoutRegistry *holder = OSDynamicCast(CTimeoutRegistry, controller->getProperty("ItlwmCTimers"));
    if (!holder || !holder->timers || !holder->drainIterator)
        return kIOReturnNotReady;
    holder->retain();
    if (holder->drainBusy) {
        holder->release();
        return kIOReturnBusy;
    }
    holder->drainBusy = true;
    struct DrainOwner {
        CTimeoutRegistry *holder;
        ~DrainOwner() {
            holder->drainBusy = false;
            holder->release();
        }
    } drainOwner{holder};
    OSCollectionIterator *iterator = holder->drainIterator;
    iterator->reset();
    bool selfActive = false;
    bool badEntry = false;
    bool unboundActive = false;
    while (OSObject *object = iterator->getNextObject()) {
        CTimeout *timeout = OSDynamicCast(CTimeout, object);
        if (!timeout) {
            badEntry = true;
            continue;
        }
        selfActive |= timeout->callbackActive && timeout->callbackThread == IOThreadSelf();
        IOWorkLoop *sourceLoop = timeout->tm ? timeout->tm->getWorkLoop() : NULL;
        if (sourceLoop && sourceLoop != loop) {
            badEntry = true;
            continue;
        }
        unboundActive |= timeout->callbackActive && !sourceLoop;
        timeout->closing = true;
        timeout->isPending = false;
        if (timeout->tm) {
            timeout->tm->cancelTimeout();
            timeout->tm->disable();
        }
    }
    if (selfActive)
        return kIOReturnBusy;
    if (badEntry)
        return kIOReturnBadArgument;
    if (unboundActive || !iterator->isValid())
        return kIOReturnNotReady;

    for (;;) {
        iterator->reset();
        CTimeout *active = NULL;
        selfActive = false;
        badEntry = false;
        unboundActive = false;
        while (OSObject *object = iterator->getNextObject()) {
            CTimeout *timeout = OSDynamicCast(CTimeout, object);
            if (!timeout) {
                badEntry = true;
                continue;
            }
            selfActive |= timeout->callbackActive && timeout->callbackThread == IOThreadSelf();
            IOWorkLoop *sourceLoop = timeout->tm ? timeout->tm->getWorkLoop() : NULL;
            if (sourceLoop && sourceLoop != loop) {
                badEntry = true;
                continue;
            }
            if (timeout->callbackActive) {
                if (!sourceLoop)
                    unboundActive = true;
                else if (!active)
                    active = timeout;
            }
        }
        if (selfActive)
            return kIOReturnBusy;
        if (badEntry)
            return kIOReturnBadArgument;
        if (unboundActive || !iterator->isValid())
            return kIOReturnNotReady;
        if (!active)
            return kIOReturnSuccess;
        active->retain();
        while (active->callbackActive) {
            AbsoluteTime deadline;
            clock_interval_to_deadline(10, kMillisecondScale,
                reinterpret_cast<uint64_t *>(&deadline));
            gate->commandSleep(active, deadline, THREAD_UNINT);
        }
        active->release();
    }
#else
    IOService *controller = OSDynamicCast(IOService, target);
    if (!controller || controller->getProperty("ItlwmTeardown") != kOSBooleanTrue)
        return kIOReturnNotReady;
    CTimeoutRegistry *holder = OSDynamicCast(CTimeoutRegistry, controller->getProperty("ItlwmCTimers"));
    if (!holder || !holder->timers)
        return kIOReturnNotReady;
    OSSet *registry = holder->timers;
    // Every object stays retained even if a sleeping callback frees its slot
    // and removes the object from the live controller registry.
    OSSet *snapshot = OSSet::withSet(registry, registry->getCount() ? registry->getCount() : 1);
    if (!snapshot)
        return kIOReturnNoMemory;
    OSCollectionIterator *iterator = OSCollectionIterator::withCollection(snapshot);
    if (!iterator) {
        snapshot->release();
        return kIOReturnNoMemory;
    }
    bool selfActive = false;
    IOReturn result = kIOReturnSuccess;
    while (OSObject *object = iterator->getNextObject()) {
        CTimeout *timeout = OSDynamicCast(CTimeout, object);
        if (!timeout) {
            result = kIOReturnBadArgument;
            continue;
        }
        timeout->closing = true;
        timeout->isPending = false;
        if (timeout->tm) {
            timeout->tm->cancelTimeout();
            timeout->tm->disable();
        }
        selfActive |= timeout->callbackActive && timeout->callbackThread == IOThreadSelf();
    }
    if (selfActive)
        result = kIOReturnBusy;
    if (result == kIOReturnSuccess) {
        iterator->reset();
        while (CTimeout *timeout = OSDynamicCast(CTimeout, iterator->getNextObject())) {
            while (timeout->callbackActive) {
                AbsoluteTime deadline;
                clock_interval_to_deadline(10, kMillisecondScale,
                    reinterpret_cast<uint64_t *>(&deadline));
                _fCommandGate->commandSleep(timeout, deadline, THREAD_UNINT);
            }
        }
    }
    iterator->release();
    snapshot->release();
    return result;
#endif
}

void CTimeout::timeoutOccurred(OSObject* owner, IOTimerEventSource* timer)
{
//    IOLog("itlwm %s\n", __FUNCTION__);
    if (owner == NULL) {
        IOLog("itlwm tm owner == NULL!!!\n");
    }
    CTimeout *tm = OSDynamicCast(CTimeout, owner);
    if (tm == NULL) {
        IOLog("itlwm tm == NULL!!!\n");
        return;
    }
    // The default native timer serializes its own action, including sleeps.
    // It retains the timer/workloop, but its owner is raw. Hold that owner here.
    if (tm->closing)
        return;
    tm->retain();
    tm->callbackActive = true;
    tm->callbackThread = IOThreadSelf();
    tm->isPending = false;
    tm->to_func(tm->to_arg);
    tm->callbackActive = false;
    tm->callbackThread = NULL;
    _fCommandGate->commandWakeup(tm, false);
    if (tm->deferredFree && !tm->foreignFreeActive) {
        tm->deferredFree = false;
        finishFree(tm);
    }
    tm->release();
}

IOReturn CTimeout::timeout_add_msec(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    IOService *controller = OSDynamicCast(IOService, target);
    if (controller && controller->getProperty("ItlwmTeardown") == kOSBooleanTrue)
        return kIOReturnNotReady;
    IOWorkLoop *wl = (IOWorkLoop*)arg1;
    int msecs = *(int*)arg2;
    CTimeout **ccto = (CTimeout **)arg0;
    
    if (ccto == NULL || *ccto == NULL) {
        return kIOReturnError;
    }
    CTimeout *cto = *ccto;
    if (cto->closing)
        return kIOReturnNotReady;
    if (cto->tm == NULL) {
        cto->tm = IOTimerEventSource::timerEventSource(cto, &CTimeout::timeoutOccurred);
        if (cto->tm == NULL) {
            return kIOReturnError;
        }
        cto->tm->enable();
        IOReturn result = wl->addEventSource(cto->tm);
        if (result != kIOReturnSuccess) {
            cto->tm->release();
            cto->tm = NULL;
            return result;
        }
    }
    cto->tm->setTimeoutMS(msecs);
    cto->isPending = true;
    return kIOReturnSuccess;
}

IOReturn CTimeout::timeout_del(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    CTimeout **ccto = (CTimeout **)arg0;
    if (ccto == NULL || *ccto == NULL) {
        return kIOReturnSuccess;
    }
    CTimeout *cto = *ccto;
    if (cto->tm == NULL || !cto->isPending) {
        return kIOReturnSuccess;
    }
    cto->tm->cancelTimeout();
    cto->isPending = false;
    return kIOReturnSuccess;
}

IOReturn CTimeout::finishFree(CTimeout *timeout)
{
    // Every admitted to_func has returned. No later delivery may touch its arg.
    timeout->to_func = NULL;
    timeout->to_arg = NULL;
    OSSet *registry = timeout->timerRegistry;
    timeout->timerRegistry = NULL;
    if (registry)
        registry->removeObject(timeout);
    if (timeout->tm) {
        IOWorkLoop *workloop = timeout->tm->getWorkLoop();
        if (workloop) {
            IOReturn result = workloop->removeEventSource(timeout->tm);
            if (result != kIOReturnSuccess) {
                IOLog("itlwm: timer removal failed; quarantining closed timer owner\n");
                // Keep the source's original reference and the original CTimeout
                // owner reference. Future native delivery sees closing first.
                return result;
            }
        }
        timeout->tm->release();
        timeout->tm = NULL;
    }
    timeout->release();
    return kIOReturnSuccess;
}

IOReturn CTimeout::timeout_free(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    CTimeout **slot = static_cast<CTimeout **>(arg0);
    if (!slot || !*slot)
        return kIOReturnSuccess;
    CTimeout *timeout = *slot;
    timeout->retain();
    timeout->closing = true;
    timeout->isPending = false;
    if (timeout->tm) {
        timeout->tm->cancelTimeout();
        timeout->tm->disable();
    }
    if (timeout->callbackActive && timeout->callbackThread == IOThreadSelf()) {
        *slot = NULL;
        timeout->slotCleared = true;
        timeout->deferredFree = true;
        timeout->release();
        return kIOReturnSuccess;
    }
    const bool finalizer = !timeout->foreignFreeActive;
    if (finalizer)
        timeout->foreignFreeActive = true;
    while (timeout->callbackActive) {
        AbsoluteTime deadline;
        clock_interval_to_deadline(10, kMillisecondScale,
            reinterpret_cast<uint64_t *>(&deadline));
        _fCommandGate->commandSleep(timeout, deadline, THREAD_UNINT);
    }
    IOReturn result = kIOReturnSuccess;
    if (finalizer) {
        // Self-free can destroy its slot owner while this frame sleeps.
        if (!timeout->slotCleared) {
            *slot = NULL;
            timeout->slotCleared = true;
        }
        result = finishFree(timeout);
    }
    timeout->release();
    return result;
}

typedef void (*callback)(void *);

IOReturn CTimeout::timeout_set(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    IOService *controller = OSDynamicCast(IOService, target);
    if (controller && controller->getProperty("ItlwmTeardown") == kOSBooleanTrue)
        return kIOReturnNotReady;
    CTimeout *tm;
    CTimeout **cto = (CTimeout **)arg0;
    if (cto == NULL) {
        return kIOReturnError;
    }
    if ((*cto) == NULL) {
        CTimeoutRegistry *holder = controller ? OSDynamicCast(CTimeoutRegistry, controller->getProperty("ItlwmCTimers")) : NULL;
        if (!holder || !holder->timers)
            return kIOReturnNotReady;
        OSSet *registry = holder->timers;
        CTimeout *created = new CTimeout;
        if (!created)
            return kIOReturnNoMemory;
        if (!created->init()) {
            created->release();
            return kIOReturnNoMemory;
        }
        if (!registry->setObject(created)) {
            created->release();
            return kIOReturnNoMemory;
        }
        created->timerRegistry = registry;
        *cto = created;
    }
    tm = *cto;
    if (!tm || tm->closing)
        return kIOReturnNotReady;
    tm->isPending = false;
    tm->to_func = (callback)arg1;
    tm->to_arg = arg2;
    return kIOReturnSuccess;
}

IOReturn CTimeout::timeout_pending(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    CTimeout **cto = (CTimeout **)arg0;
    CTimeout *tm;
    if (cto == NULL) {
        return kIOReturnError;
    }
    tm = *cto;
    if (tm != NULL && !tm->closing && tm->isPending) {
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}
