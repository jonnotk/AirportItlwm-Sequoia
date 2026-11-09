// Included once per controller implementation with ITL_CONTROLLER_CLASS and
// ITL_CONTROLLER_HAS_SCAN defined. Default native timers retain their source
// and workloop, but their action receives a borrowed controller pointer.
void ITL_CONTROLLER_CLASS::closeControllerTimerAdmission()
{
    if (!fControllerTimerLock) return;
    IOLockLock(fControllerTimerLock);
    fControllerTimerClosing = true;
    IOLockUnlock(fControllerTimerLock);
}

bool ITL_CONTROLLER_CLASS::holdControllerTimerOwner()
{
    if (!fControllerTimerLock) return false;
    IOLockLock(fControllerTimerLock);
    const bool open = !fControllerTimerClosing;
    if (open && !fControllerTimerOwnerHeld) {
        retain();
        fControllerTimerOwnerHeld = true;
    }
    IOLockUnlock(fControllerTimerLock);
    return open;
}

bool ITL_CONTROLLER_CLASS::beginControllerTimer(ControllerTimerFrame *frame,
    IOTimerEventSource *source, bool scan)
{
    if (!fControllerTimerLock) return false;
    IOLockLock(fControllerTimerLock);
    bool matches = !scan && source && source == watchdogTimer;
#if ITL_CONTROLLER_HAS_SCAN
    matches |= scan && source && source == scanSource;
#endif
    if (fControllerTimerClosing || !matches) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    frame->thread = current_thread();
    frame->next = fControllerTimerFrames;
    frame->source = source;
    frame->rearm = !scan;
    frame->owner = this;
    fControllerTimerFrames = frame;
    IOLockUnlock(fControllerTimerLock);
    return true;
}

ITL_CONTROLLER_CLASS::ControllerTimerFrame::~ControllerTimerFrame()
{
    if (owner) owner->endControllerTimer(this);
}

void ITL_CONTROLLER_CLASS::endControllerTimer(ControllerTimerFrame *frame)
{
    IOLockLock(fControllerTimerLock);
    auto **link = &fControllerTimerFrames;
    while (*link && *link != frame) link = &(*link)->next;
    if (*link) *link = frame->next;
    if (!fControllerTimerClosing && frame->rearm && frame->source == watchdogTimer)
        frame->source->setTimeoutMS(kWatchDogTimerPeriod);
    IOLockWakeup(fControllerTimerLock, &fControllerTimerFrames, false);
    IOLockUnlock(fControllerTimerLock);
}

bool ITL_CONTROLLER_CLASS::quiesceControllerTimers()
{
    if (!fControllerTimerLock) return true;
    IOLockLock(fControllerTimerLock);
    fControllerTimerClosing = true;
    if (!fRetiredWatchdogTimer) fRetiredWatchdogTimer = watchdogTimer;
    watchdogTimer = NULL;
#if ITL_CONTROLLER_HAS_SCAN
    if (!fRetiredScanTimer) fRetiredScanTimer = scanSource;
    scanSource = NULL;
#endif
    if (fControllerTimerDraining) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    auto *watchdog = fRetiredWatchdogTimer;
    auto *scan = fRetiredScanTimer;
    if (watchdog) { watchdog->cancelTimeout(); watchdog->disable(); }
    if (scan) { scan->cancelTimeout(); scan->disable(); }
    // No registered source and no entered frame needs a gate transition.
    // This also makes partial allocation and repeated cleanup safe in-gate.
    if (!fWatchdogTimerAdded && !fScanSourceAdded && !fControllerTimerFrames) {
        IOLockUnlock(fControllerTimerLock);
        return true;
    }
    bool self = false;
    for (auto *frame = fControllerTimerFrames; frame; frame = frame->next)
        self |= frame->thread == current_thread();
    // Never block foreign-workloop removal while holding a gate needed by an
    // entered action. Self/gated callers preserve the lease for external retry.
    if (self || (_fWorkloop && _fWorkloop->inGate()) ||
        (fWatchdogWorkLoop && fWatchdogWorkLoop->inGate())) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fControllerTimerDraining = true;
    IOLockUnlock(fControllerTimerLock);
    bool removed = true;
    if (watchdog && fWatchdogTimerAdded) {
        if (!fWatchdogWorkLoop ||
            fWatchdogWorkLoop->removeEventSource(watchdog) != kIOReturnSuccess) removed = false;
        else fWatchdogTimerAdded = false;
    }
#if ITL_CONTROLLER_HAS_SCAN
    if (scan && fScanSourceAdded) {
        if (!_fWorkloop || _fWorkloop->removeEventSource(scan) != kIOReturnSuccess) removed = false;
        else fScanSourceAdded = false;
    }
#endif
    IOLockLock(fControllerTimerLock);
    // Successful unregister excludes the native pre-entry raw-owner window.
    // Entered actions can outlive removal if they released their source gate.
    if (removed) {
        while (fControllerTimerFrames)
            IOLockSleep(fControllerTimerLock, &fControllerTimerFrames, THREAD_UNINT);
    }
    fControllerTimerDraining = false;
    IOLockUnlock(fControllerTimerLock);
    return removed;
}

bool ITL_CONTROLLER_CLASS::releaseControllerTimerReferences()
{
    if (!fControllerTimerLock) return true;
    IOLockLock(fControllerTimerLock);
    if (!fControllerTimerClosing || fControllerTimerDraining || fControllerTimerFrames ||
        fWatchdogTimerAdded || fScanSourceAdded) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    auto *watchdog = fRetiredWatchdogTimer;
    auto *scan = fRetiredScanTimer;
    fRetiredWatchdogTimer = fRetiredScanTimer = NULL;
    const bool held = fControllerTimerOwnerHeld;
    fControllerTimerOwnerHeld = false;
    IOLockUnlock(fControllerTimerLock);
    // Kept through child stop because the native child borrows the scan timer.
    if (scan) scan->release();
    if (watchdog) watchdog->release();
    if (held) release();
    return true;
}

bool ITL_CONTROLLER_CLASS::controllerTimersClosing()
{
    if (!fControllerTimerLock) return true;
    IOLockLock(fControllerTimerLock);
    const bool closing = fControllerTimerClosing;
    IOLockUnlock(fControllerTimerLock);
    return closing;
}

bool ITL_CONTROLLER_CLASS::armControllerWatchdog()
{
    if (!fControllerTimerLock) return false;
    IOLockLock(fControllerTimerLock);
    const bool open = !fControllerTimerClosing && watchdogTimer;
    if (open) {
        watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
        watchdogTimer->enable();
    }
    IOLockUnlock(fControllerTimerLock);
    return open;
}
