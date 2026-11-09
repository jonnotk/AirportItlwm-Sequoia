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

#include "ItlHalService.hpp"
#include <sys/CTimeout.hpp>

#define super OSObject
OSDefineMetaClassAndAbstractStructors(ItlHalService, OSObject)

bool ItlHalService::
initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate)
{
    if (teardownStarted || !controller || !workloop || !commandGate ||
        controller->getProperty("ItlwmTeardown") == kOSBooleanTrue || this->controller ||
        mainWorkLoop || mainCommandGate || inner_attr || inner_gp_attr ||
        inner_gp || inner_lock)
        return false;
    if (!super::init())
        return false;
    inner_attr = lck_attr_alloc_init();
    if (!inner_attr)
        return false;
    inner_gp_attr = lck_grp_attr_alloc_init();
    if (inner_gp_attr)
        inner_gp = lck_grp_alloc_init("itlwm_tsleep", inner_gp_attr);
    if (inner_gp)
        inner_lock = lck_mtx_alloc_init(inner_gp, inner_attr);
    if (!inner_lock || !controller->setProperty("ItlwmTeardown", kOSBooleanFalse) ||
        !CTimeout::initializeControllerTimers(controller)) {
        if (inner_lock) lck_mtx_free(inner_lock, inner_gp);
        inner_lock = NULL;
        if (inner_gp) lck_grp_free(inner_gp);
        if (inner_gp_attr) lck_grp_attr_free(inner_gp_attr);
        lck_attr_free(inner_attr);
        inner_gp = NULL;
        inner_gp_attr = NULL;
        inner_attr = NULL;
        return false;
    }
    this->controller = controller;
    controller->retain();
    mainWorkLoop = workloop;
    workloop->retain();
    mainCommandGate = commandGate;
    commandGate->retain();
    return true;
}

IOEthernetController *ItlHalService::
getController()
{
    return this->controller;
}

IOCommandGate *ItlHalService::
getMainCommandGate()
{
    return this->mainCommandGate;
}

IOWorkLoop *ItlHalService::
getMainWorkLoop()
{
    return this->mainWorkLoop;
}

void ItlHalService::
wakeupOn(void *ident)
{
//    XYLog("%s\n", __FUNCTION__);
    wakeup(ident);
}

int ItlHalService::
tsleep_nsec(void *ident, int priority, const char *wmesg, int timo)
{
//    XYLog("%s %s\n", __FUNCTION__, wmesg);
    struct timespec ts;
    int err;
    memset(&ts, 0, sizeof(struct timespec));
    ts.tv_nsec = timo;
    lck_mtx_lock(this->inner_lock);
    err = msleep(ident, this->inner_lock, priority, wmesg, &ts);
    lck_mtx_unlock(this->inner_lock);
    return err;
}

void ItlHalService::
free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (this->mainWorkLoop) {
        this->mainWorkLoop->release();
    }
    this->mainWorkLoop = NULL;
    if (this->mainCommandGate) {
        this->mainCommandGate->release();
    }
    this->mainCommandGate = NULL;
    if (this->controller) {
        this->controller->release();
    }
    if (inner_lock) lck_mtx_free(inner_lock, inner_gp);
    if (inner_gp) lck_grp_free(inner_gp);
    if (inner_gp_attr) lck_grp_attr_free(inner_gp_attr);
    if (inner_attr) lck_attr_free(inner_attr);
    inner_lock = NULL;
    inner_gp = NULL;
    inner_gp_attr = NULL;
    inner_attr = NULL;
    this->controller = NULL;
    super::free();
}

void ItlHalService::acquireInterruptSourceLease()
{
    if (!interruptSourceLease) {
        retain();
        interruptSourceLease = true;
    }
}

bool ItlHalService::releaseInterruptSource(IOInterruptEventSource *&source)
{
    IOInterruptEventSource *interrupt = source;
    if (interrupt) {
        interrupt->disable();
        IOWorkLoop *workloop = interrupt->getWorkLoop();
        if (workloop && workloop->removeEventSource(interrupt) != kIOReturnSuccess) {
            teardownQuarantined = true;
            IOLog("itlwm: interrupt removal failed; retaining HAL and device resources\n");
            return false;
        }
        source = NULL;
        // Removal unregisters the provider interrupt. Keep the HAL lease and
        // mapped registers alive until the final source release also completes.
        interrupt->release();
    }
    if (interruptSourceLease) {
        interruptSourceLease = false;
        release();
    }
    return true;
}

bool ItlHalService::beginTeardown()
{
    teardownStarted = true;
    if (controller && controller->setProperty("ItlwmTeardown", kOSBooleanTrue))
        return true;
    quarantineTeardown();
    return false;
}

void ItlHalService::quarantineTeardown()
{
    teardownQuarantined = true;
    if (!teardownQuarantineLease) {
        retain();
        teardownQuarantineLease = true;
    }
}

IOReturn ItlHalService::quiesceGated(OSObject *, void *arg, void *, void *, void *)
{
    ItlHalService *self = static_cast<ItlHalService *>(arg);
    return self && self->quiesceForDetach() ? kIOReturnSuccess : kIOReturnNotReady;
}
