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

#include "itlwm.hpp"

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void itlwm::beginPowerCall(PowerCallFrame *frame)
{
    IOLockLock(fPowerCallLock);
    frame->thread = current_thread();
    frame->next = fPowerCallFrames;
    fPowerCallFrames = frame;
    IOLockUnlock(fPowerCallLock);
}

void itlwm::endPowerCall(PowerCallFrame *frame)
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

bool itlwm::unregistPM(bool permanent)
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

IOReturn itlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
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

IOReturn itlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static IOReturn handleSetPowerStateOffGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = OSDynamicCast(itlwm, owner);
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
    auto *self = OSDynamicCast(itlwm, owner);
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
    auto *self = static_cast<itlwm *>(param0);
    itlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    if (gate) gate->runAction(handleSetPowerStateOffGated);
    self->endPowerCall(&frame);
    self->release();
}

static void handleSetPowerStateOn(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<itlwm *>(param0);
    itlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    if (gate) gate->runAction(handleSetPowerStateOnGated);
    self->endPowerCall(&frame);
    self->release();
}

IOReturn itlwm::registerWithPolicyMaker(IOService *policyMaker)
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

void itlwm::setPowerStateOff()
{
    IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOff;
    IOLockUnlock(fPowerCallLock);
    if (pmPolicyMaker) pmPolicyMaker->acknowledgeSetPowerState();
}

void itlwm::setPowerStateOn()
{
    IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOn;
    IOLockUnlock(fPowerCallLock);
    if (pmPolicyMaker) pmPolicyMaker->acknowledgeSetPowerState();
}
