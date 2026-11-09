//
//  CTimeout.h
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

#ifndef CTimeout_h
#define CTimeout_h

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSet.h>
#include <libkern/c++/OSObject.h>

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
class OSCollectionIterator;
#endif

// IORegistry freezes collection-valued properties. Publish this ordinary
// OSObject holder, whose private mutable set is never a property value.
class CTimeoutRegistry : public OSObject {
    OSDeclareDefaultStructors(CTimeoutRegistry)
public:
    virtual bool init() override;
    OSSet *timers = NULL;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    OSCollectionIterator *drainIterator = NULL;
    bool drainBusy = false;
#endif
    virtual void free() override;
};

class CTimeout : public OSObject {
    OSDeclareDefaultStructors(CTimeout)
    
public:
    virtual bool init() override;

    static bool initializeControllerTimers(IOService *controller);
    static IOReturn quiesceControllerTimers(OSObject *target, void *, void *, void *, void *);

    static void timeoutOccurred(OSObject* owner, IOTimerEventSource* timer);
    
    static IOReturn timeout_add_msec(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn timeout_del(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn timeout_free(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn timeout_set(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn timeout_pending(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
public:
    IOTimerEventSource* tm = NULL;
    void (*to_func)(void *) = NULL;        /* function to call */
    void *to_arg = NULL;                /* function argument */
    bool isPending = false;
    bool closing = false;
    bool callbackActive = false;
    bool deferredFree = false;
    bool foreignFreeActive = false;
    bool slotCleared = false;
    thread_t callbackThread = NULL;
    OSSet *timerRegistry = NULL;

private:
    static IOReturn finishFree(CTimeout *timeout);
};

#endif /* CTimeout_h */
