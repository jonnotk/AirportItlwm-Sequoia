//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>
#include <libkern/OSAtomic.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "IOPCIEDeviceWrapper.hpp"
#if __IO80211_TARGET >= __MAC_15_0
#include <Airport/IO80211FaultReporter.h>
#endif

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMapper.h>
#include <HAL/AiritlDebug.h>

// Gated debug logging (see include/HAL/AiritlDebug.h): informational traces
// print only when the -airitldbg boot-arg is set.
extern "C" {
int airitl_debug_log = 0;
}

// The Skywalk datapath provision and the interface registration are both
// compiled out unless these are defined, and nothing in the project defines
// them: without them the interface attaches and starts but is never registered,
// so no BSD unit and no Wi-Fi port ever appear. Scope them to the selected
// MAC15 non-BSDIF build so the older and BSDIF variants keep their behavior.
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
#ifndef AIRITL_DATAPATH_P0
#define AIRITL_DATAPATH_P0
#endif
#ifndef AIRITL_DP_REGISTER
#define AIRITL_DP_REGISTER
#endif
#endif

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

// --- DIAGNOSTIC EXPERIMENT: replicate RangingManager's failing timer creation ---
// super::start fails because IO80211RangingManager::initWithController's session
// IO80211TimerSource::allocWithParams(2, owner, getWorkQueue(), cb) returns null. Static analysis
// says it should succeed; our workqueue is the only driver-supplied input. This pre-creates the SAME
// timer with our _fWorkloop to test whether the workqueue can back a timer at all.
class IO80211TimerSource;
enum IO80211TimerSourcePriority { kIO80211TimerSourcePriorityNormal = 2 };
class IO80211TimerSource {
public:
    static IO80211TimerSource *allocWithParams(IO80211TimerSourcePriority, OSObject *,
                                               IO80211WorkQueue *, void (*)(OSObject *, IO80211TimerSource *));
};
static void airitl_timer_test_cb(OSObject *, IO80211TimerSource *) { }

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;
static AirportItlwm *gActiveController = NULL;

// Diagnostic: true only while super::start() runs, so our overrides can record whether Apple's
// IO80211Controller::start called them during start (vs async noise). Helps narrow where start fails.
static bool g_airitlInSuperStart = false;
static AirportItlwm *g_airitlSelf = NULL;
static void airitl_ss_hit(const char *key) {
    if (g_airitlInSuperStart && g_airitlSelf && g_airitlSelf->getProvider())
        g_airitlSelf->getProvider()->setProperty(key, "yes");
}

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)

struct AirportItlwm::FailedStartCleanupFrame {
    enum class Stage { Reserved, Claimed, Drained, ChildDisposed, ParentStopped, Finished };
    AirportItlwm *owner = NULL;
    IOService *provider = NULL;
    AirportItlwmSkywalkInterface *child = NULL; // Independent positive child reference.
    thread_t caller = NULL;
    IOWorkLoop *loop = NULL;
    IOCommandGate *gate = NULL, *nativeGate = NULL;
    ItlHalService *hal = NULL;
    IOPCIDevice *pci = NULL;
    IOTimerEventSource *watchdog = NULL, *scan = NULL;
    OSObject *expected[6] = {};
    OSObject *leases[8] = {};
    Stage stage = Stage::Reserved;
    IOReturn status = kIOReturnNotReady;
    bool published = false, claimed = false, quarantined = false, finished = false;
    void hold(unsigned slot, OSObject *object) {
        leases[slot] = object;
        if (object) object->retain();
    }
    ~FailedStartCleanupFrame() {
        if (owner) {
            IOLockLock(owner->fControllerTimerLock);
            if (!finished) {
                if (!quarantined) { owner->retain(); quarantined = true; }
                owner->fStartInProgress = true;
                owner->fStopping = true;
                owner->fControllerTimerClosing = true;
            }
            if (owner->fFailedStartCleanup == this) owner->fFailedStartCleanup = NULL;
            published = false;
            const bool fault = !finished;
            const unsigned lastStage = static_cast<unsigned>(stage);
            const IOReturn lastStatus = status;
            IOLockUnlock(owner->fControllerTimerLock);
            if (fault) IOLog("AirportItlwm direct startup unresolved stage %u status 0x%x\n",
                             lastStage, lastStatus);
        }
        auto *heldChild = child; child = NULL;
        if (heldChild) heldChild->release();
        // HAL destruction precedes its PCI, timer, gate, loop and provider support.
        for (unsigned i = 0; i < 8; ++i) {
            OSObject *object = leases[i]; leases[i] = NULL;
            if (object) object->release();
        }
        if (finished && quarantined) {
            quarantined = false;
            owner->release();
        }
    }
};

bool AirportItlwm::directFailedStartCallerReady(IOService *provider, IOWorkLoop *loop, thread_t caller)
{
    if (!provider || !loop || !caller || caller != current_thread() ||
        loop->inGate() || loop->onThread() || !fControllerTimerLock || !fPowerCallLock)
        return false;
    auto *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper || wrapper->getWorkLoop() != loop || loop != _fWorkloop ||
        getProvider() != provider)
        return false;
    IOLockLock(fControllerTimerLock);
    bool ready = fNativeInitComplete;
    for (auto *frame = fControllerTimerFrames; frame; frame = frame->next)
        if (frame->thread == caller) ready = false;
    const bool nativeStarted = fNativeStartPhase == NativeStartPhase::Succeeded;
    if (nativeStarted)
        ready &= fStartInProgress && fControllerStarted && fWrapperProvider == wrapper;
    else
        ready &= fNativeStartPhase == NativeStartPhase::NotAttempted &&
            !fStartInProgress && !fControllerTimerClosing && !fControllerStarted && !fNetIf && !fCleanupInProgress;
    IOLockUnlock(fControllerTimerLock);
    IOLockLock(fPowerCallLock);
    for (auto *frame = fPowerCallFrames; frame; frame = frame->next)
        if (frame->thread == caller) ready = false;
    IOLockUnlock(fPowerCallLock);
    if (nativeStarted) {
        auto *nativeGate = getCommandGate();
        ready &= nativeGate && nativeGate->getWorkLoop() == loop &&
            _fCommandGate && _fCommandGate->getWorkLoop() == loop &&
            fCommandGateAdded && fCommandGateAcquired && fWatchdogWorkLoop == loop;
    }
    return ready;
}

bool AirportItlwm::cleanupFailedNativeStart(IOService *provider,
    AirportItlwmSkywalkInterface *child, IOWorkLoop *loop, thread_t caller)
{
    FailedStartCleanupFrame frame;
    frame.owner = this; frame.provider = provider; frame.loop = loop; frame.caller = caller;
    // Reserve one positive reference now; fault adoption precedes every cleanup epilogue.
    retain(); frame.quarantined = true;
    frame.nativeGate = getCommandGate();
    closeControllerTimerAdmission();
    IOLockLock(fControllerTimerLock);
    frame.hal = fHalService; frame.pci = pciNub; frame.gate = _fCommandGate;
    frame.watchdog = watchdogTimer ? watchdogTimer : fRetiredWatchdogTimer;
    frame.scan = scanSource ? scanSource : fRetiredScanTimer;
    frame.hold(0, frame.hal); frame.hold(1, frame.pci);
    frame.hold(2, frame.watchdog); frame.hold(3, frame.scan);
    frame.hold(4, frame.gate); frame.hold(5, frame.nativeGate);
    frame.hold(6, loop); frame.hold(7, provider);
    if (child && fNetIf == child) {
        frame.child = child; child->retain();
        frame.expected[0] = child->fRxCompQueue; frame.expected[1] = child->fRxSubQueue;
        frame.expected[2] = child->fTxCompQueue; frame.expected[3] = child->fTxSubQueue;
        frame.expected[4] = child->fMCastQueue; frame.expected[5] = child->fPacketPool;
    }
    IOLockUnlock(fControllerTimerLock);
    if (!frame.child || !OSDynamicCast(ItlIwx, frame.hal) ||
        !directFailedStartCallerReady(provider, loop, caller) ||
        getCommandGate() != frame.nativeGate) return false;
    IOLockLock(fControllerTimerLock);
    const bool reserve = fStartInProgress && !fFailedStartCleanup && fControllerStarted &&
        fNativeStartPhase == NativeStartPhase::Succeeded && fHalService == frame.hal &&
        pciNub == frame.pci && fNetIf == frame.child && fWrapperProvider == provider &&
        _fWorkloop == loop && _fCommandGate == frame.gate && fHalAttached && fHalAttachAttempted &&
        frame.watchdog && frame.scan &&
        (!watchdogTimer || watchdogTimer == frame.watchdog) &&
        (!fRetiredWatchdogTimer || fRetiredWatchdogTimer == frame.watchdog) &&
        (!scanSource || scanSource == frame.scan) &&
        (!fRetiredScanTimer || fRetiredScanTimer == frame.scan);
    if (reserve) {
        fFailedStartCleanup = &frame; frame.published = true;
        fStopping = true;
    }
    IOLockUnlock(fControllerTimerLock);
    if (!reserve) return false;
    // The publication mutex is released before the one authorized cleanup call.
    const bool complete = cleanup(provider, false, &frame);
    return complete && frame.finished;
}

#endif

void AirportItlwm::releaseAll()
{
    if (!fNativeInitComplete) return;
    cleanup(getProvider());
}

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
bool AirportItlwm::cleanup(IOService *provider, bool freeing, FailedStartCleanupFrame *authority)
#else
bool AirportItlwm::cleanup(IOService *provider, bool freeing)
#endif
{
    if (!fNativeInitComplete) return false;
    // Final release cannot create a positive owner for a pending child action.
    // Ordinary cleanup must finish and consume the structural child first.
    if (freeing && fNetIf) return false;
    struct Lifetime {
        AirportItlwm *owner;
        bool held;
        ~Lifetime() { if (held) owner->release(); }
    } lifetime{this, !freeing};
    if (!freeing) retain();
    closeControllerTimerAdmission();
    if (fControllerTimerLock) IOLockLock(fControllerTimerLock);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    bool direct = false;
    fStopping = true;
    if (authority) {
        auto exactAuthority = [&]() {
            return fControllerTimerLock && !freeing && authority == fFailedStartCleanup &&
                authority->owner == this && authority->provider == provider &&
                authority->caller == current_thread() && authority->published &&
                !authority->claimed && authority->stage == FailedStartCleanupFrame::Stage::Reserved &&
                fStartInProgress && fStopping && fControllerTimerClosing &&
                fWrapperProvider == provider && fNetIf == authority->child &&
                fHalService == authority->hal && _fCommandGate == authority->gate &&
                _fWorkloop == authority->loop && fControllerStarted &&
                fNativeStartPhase == NativeStartPhase::Succeeded;
        };
        if (!exactAuthority()) {
            if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
            return false;
        }
        while (fCleanupInProgress) {
            IOLockSleep(fControllerTimerLock, &fCleanupInProgress, THREAD_UNINT);
            if (!exactAuthority()) { IOLockUnlock(fControllerTimerLock); return false; }
        }
        direct = true;
        authority->claimed = true;
        authority->stage = FailedStartCleanupFrame::Stage::Claimed;
    } else if (fFailedStartCleanup) {
        if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
        return false;
    }
#endif
    if (fCleanupInProgress) {
        if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fCleanupInProgress = true;
    fStopping = true;
    const bool starting = fStartInProgress;
    if (fControllerTimerLock) IOLockUnlock(fControllerTimerLock);
    struct CleanupOwner {
        AirportItlwm *owner;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
        FailedStartCleanupFrame *authority;
#endif
        ~CleanupOwner() {
            if (owner->fControllerTimerLock) IOLockLock(owner->fControllerTimerLock);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
            if (authority && !authority->finished && authority->status == kIOReturnSuccess)
                authority->status = kIOReturnNotReady;
#endif
            owner->fCleanupInProgress = false;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
            if (owner->fControllerTimerLock)
                IOLockWakeup(owner->fControllerTimerLock, &owner->fCleanupInProgress, false);
#endif
            if (owner->fControllerTimerLock) IOLockUnlock(owner->fControllerTimerLock);
        }
    } cleanupOwner{this
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
        , direct ? authority : NULL
#endif
    };
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (starting && !direct) return false;
#else
    if (starting) return false;
#endif
    if (fNativeStartPhase == NativeStartPhase::InProgress ||
        fNativeStartPhase == NativeStartPhase::Failed) return false;
    // Inspect owned phases before draining any dependency of a partial child.
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (!direct && fNetIf) {
#else
    if (fNetIf) {
#endif
        auto *child = OSDynamicCast(AirportItlwmSkywalkInterface, fNetIf);
        fNetIf->retain();
        const bool ready = child && (fCommandGateAdded && _fCommandGate ?
            _fCommandGate->runAction(nativeChildCleanupReadyGated, child) == kIOReturnSuccess :
            (!fInterfaceAttached && !fControllerInterfaceAttached && child->controllerCleanupReady() &&
             child->datapathTeardownComplete()));
        fNetIf->release();
        if (!ready) return false;
    }
    if (g_airitlSelf == this) {
        g_airitlSelf = NULL;
        g_airitlInSuperStart = false;
    }

#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (direct) {
        IOReturn fresh = authority->gate->runAction(
            [](OSObject *object, void *arg, void *, void *, void *) -> IOReturn {
                auto *that = OSDynamicCast(AirportItlwm, object);
                auto *frame = static_cast<FailedStartCleanupFrame *>(arg);
                if (!that || frame->owner != that || that->fNetIf != frame->child ||
                    that->fHalService != frame->hal || that->pciNub != frame->pci ||
                    _fCommandGate != frame->gate || _fWorkloop != frame->loop ||
                    frame->child->fRxCompQueue != frame->expected[0] ||
                    frame->child->fRxSubQueue != frame->expected[1] ||
                    frame->child->fTxCompQueue != frame->expected[2] ||
                    frame->child->fTxSubQueue != frame->expected[3] ||
                    frame->child->fMCastQueue != frame->expected[4] ||
                    frame->child->fPacketPool != frame->expected[5]) return kIOReturnNotReady;
                return frame->child->validateFailedNativeStartAbort(that) ?
                    kIOReturnSuccess : kIOReturnNotReady;
            }, authority);
        authority->status = fresh;
        if (fresh != kIOReturnSuccess) return false;
    }
#endif
    const bool pmQuiesced = unregistPM();
    const bool timersQuiesced = quiesceControllerTimers();
#ifdef AIRITL_LINK_DEFER
    const bool linkQuiesced = quiesceLinkStateCall();
#else
    const bool linkQuiesced = true;
#endif
    if (!pmQuiesced || !timersQuiesced || !linkQuiesced) {
        return false;
    }
    if (fControllerOwnerClaimed && _fWorkloop && _fWorkloop->inGate()) return false;
    // Cancel local native work while recipients live. The final-stop action
    // delegates hardware/CTO/IRQ closure to the non-destructive HAL preflight.
    IOReturn quiesced = kIOReturnSuccess;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (direct) {
        if (fHalService != authority->hal || _fCommandGate != authority->gate ||
            _fWorkloop != authority->loop) return false;
        quiesced = authority->hal && authority->hal->quiesceForDetach() ?
            kIOReturnSuccess : kIOReturnNotReady;
        authority->status = quiesced;
    } else
#endif
    if (fHalAttached && fHalService && _fCommandGate && fCommandGateAdded)
        quiesced = _fCommandGate->runAction(nativeQuiesceGated, (void *)1);
    else if (fHalService && !fHalService->quiesceForDetach())
        quiesced = kIOReturnNotReady;
    if (quiesced != kIOReturnSuccess ||
        (fHalService && fHalService->teardownMustRetainResources())) {
        return false;
    }
    struct ieee80211com *ic = fHalService ? fHalService->get80211Controller() : NULL;
    if (ic && ic->ic_event_handler == eventHandler)
        ic->ic_event_handler = NULL;
    OSSafeReleaseNULL(fTimerProbe);

    // Keep dependencies alive until framework stop, provider close and detach
    // actually finish. A synchronous request can enqueue or time out.
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (direct) {
        IOLockLock(fControllerTimerLock);
        authority->stage = FailedStartCleanupFrame::Stage::Drained;
        IOLockUnlock(fControllerTimerLock);
        auto *child = authority->child;
        AirportItlwmSkywalkInterface::FailedNativeStartOwners owners;
        IOReturn moved = authority->gate->runAction(
            [](OSObject *object, void *arg, void *out, void *, void *) -> IOReturn {
                auto *that = OSDynamicCast(AirportItlwm, object);
                auto *frame = static_cast<FailedStartCleanupFrame *>(arg);
                if (!that || frame->owner != that || that->fNetIf != frame->child ||
                    that->fHalService != frame->hal || that->pciNub != frame->pci ||
                    _fWorkloop != frame->loop || _fCommandGate != frame->gate ||
                    that->fRetiredScanTimer != frame->scan ||
                    that->fRetiredWatchdogTimer != frame->watchdog ||
                    that->watchdogTimer || that->scanSource || that->fControllerTimerFrames ||
                    that->fWatchdogTimerAdded || that->fScanSourceAdded ||
                    frame->child->fRxCompQueue != frame->expected[0] ||
                    frame->child->fRxSubQueue != frame->expected[1] ||
                    frame->child->fTxCompQueue != frame->expected[2] ||
                    frame->child->fTxSubQueue != frame->expected[3] ||
                    frame->child->fMCastQueue != frame->expected[4] ||
                    frame->child->fPacketPool != frame->expected[5])
                    return kIOReturnNotReady;
                return frame->child->takeFailedNativeStartOwners(
                    static_cast<AirportItlwmSkywalkInterface::FailedNativeStartOwners *>(out)) ?
                    kIOReturnSuccess : kIOReturnNotReady;
            }, authority, &owners);
        authority->status = moved;
        if (moved != kIOReturnSuccess) return false;
        // The complete transfer is followed by all six nonfallible drops.
        { auto *p = owners.rxCompletion; owners.rxCompletion = NULL; if (p) p->release(); }
        { auto *p = owners.rxSubmission; owners.rxSubmission = NULL; if (p) p->release(); }
        { auto *p = owners.txCompletion; owners.txCompletion = NULL; if (p) p->release(); }
        { auto *p = owners.txSubmission; owners.txSubmission = NULL; if (p) p->release(); }
        { auto *p = owners.multicast; owners.multicast = NULL; if (p) p->release(); }
        { auto *p = owners.pool; owners.pool = NULL; if (p) p->release(); }
        if (fNetIf != child || child->getProvider() != this ||
            !fControllerInterfaceAttached || !fInterfaceAttached) return false;
        detachInterface(child, false);
        fControllerInterfaceAttached = false;
        if (child->getProvider() != this) return false;
        child->detach(this);
        IOReturn detached = authority->gate->runAction(
            [](OSObject *object, void *arg, void *, void *, void *) -> IOReturn {
                auto *that = OSDynamicCast(AirportItlwm, object);
                auto *frame = static_cast<FailedStartCleanupFrame *>(arg);
                if (!that || frame->owner != that || that->fNetIf != frame->child ||
                    frame->child->getProvider() || that->fControllerInterfaceAttached)
                    return kIOReturnNotReady;
                that->fInterfaceAttached = false;
                if (!frame->child->finishFailedNativeStartDetach(that)) return kIOReturnNotReady;
                that->fInterfacePublished = false;
                that->fNetIf = NULL;
                return kIOReturnSuccess;
            }, authority);
        authority->status = detached;
        if (detached != kIOReturnSuccess) return false;
        child->release(); // Consume the removed initial fNetIf reference.
        authority->child = NULL;
        child->release(); // Consume the independent child lease. No later child access.
        IOLockLock(fControllerTimerLock);
        authority->stage = FailedStartCleanupFrame::Stage::ChildDisposed;
        IOLockUnlock(fControllerTimerLock);
    } else
#endif
    if (fNetIf) {
        auto *child = fNetIf;
        child->retain();
        auto *childProvider = child->getProvider();
        const bool wasAttached = fInterfaceAttached || fControllerInterfaceAttached;
        if (childProvider && childProvider != this) { child->release(); return false; }
        const bool neverAttached = !childProvider && !wasAttached && !fChildTerminationRequested;
        if (childProvider == this && !fChildTerminationRequested) {
            fInterfaceAttached = true;
            fChildTerminationRequested = true;
            child->terminate(kIOServiceRequired | kIOServiceSynchronous);
        }
        // Termination is requested once. Provider loss alone does not discharge
        // a native stop that never ran, nor any remaining local obligations.
        bool stopped = false;
        if (!child->getProvider() && _fCommandGate && fCommandGateAdded) {
            const bool finished = _fCommandGate->runAction(nativeChildFinishCleanupGated,
                child, neverAttached ? (void *)1 : NULL) == kIOReturnSuccess;
            stopped = finished && _fCommandGate->runAction(nativeChildStopCompleteGated,
                child) == kIOReturnSuccess;
        } else if (neverAttached) {
            auto *customChild = OSDynamicCast(AirportItlwmSkywalkInterface, child);
            // No gate exists only on an empty initialized never-started path.
            // Do not use this fallback to dispose a graph with bound work.
            stopped = customChild && customChild->finishUnstartedCleanup() &&
                customChild->controllerStopComplete();
        }
        child->release();
        if (!stopped) return false;
        // The native V2 wrapper clears only its borrowed primary pointer.
        if (fControllerInterfaceAttached) detachInterface(fNetIf, false);
        fInterfacePublished = false;
        fControllerInterfaceAttached = false;
        fInterfaceAttached = false;
    } else if (fInterfaceAttached || fControllerInterfaceAttached) return false;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (direct && (getProvider() != provider || fWrapperProvider != provider ||
        !fControllerStarted || fNativeStartPhase != NativeStartPhase::Succeeded)) return false;
#endif
    if (fControllerStarted) {
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
        if (!direct) fControllerStarted = false;
        super::stop(provider);
        if (direct) {
            fControllerStarted = false;
            IOLockLock(fControllerTimerLock);
            authority->stage = FailedStartCleanupFrame::Stage::ParentStopped;
            IOLockUnlock(fControllerTimerLock);
        }
#else
        fControllerStarted = false;
        super::stop(provider);
#endif
    }
    if (fEthernetAttached && ic) {
        fEthernetAttached = false;
        ether_ifdetach(&ic->ic_ac.ac_if);
    }
    if (fHalAttachAttempted && fHalService) {
        fHalService->detach(pciNub);
        if (fHalService->teardownMustRetainResources()) {
            return false;
        }
        fHalAttached = fHalAttachAttempted = false;
    }
    if (fNativeHooksInstalled && _fCommandGate && fCommandGateAdded)
        _fCommandGate->runAction(nativeHooksGated, NULL);
    // The retained HAL object owns ic as an embedded member. Detach drains its
    // tasks and interrupts but does not release the HAL object itself.
    if (fHalService && fHalService->get80211Controller() &&
        fHalService->get80211Controller()->ic_ac.ac_if.netStat == fpNetStats)
        fHalService->get80211Controller()->ic_ac.ac_if.netStat = NULL;
    fpNetStats = NULL;
    // The child borrows scanTimer. Keep it alive until clients and HAL callbacks
    // have drained and interface stop has cleared the borrowed reference.
    if (!releaseControllerTimerReferences()) {
        return false;
    }
    // Keep our interface reference until HAL detach has drained its callbacks.
    OSSafeReleaseNULL(fNetIf);
    fNativeBootstrapDispatched = false;
    fNativeResetActive = false;
    bzero(&fNativeReset, sizeof(fNativeReset));
    fNativeAvailabilityDispatched = false;
    fNativeCoreAvailable = false;
    fNativeCoreReason = 0;
    fNativeCoreFlags = 0;
    bsdInterface = NULL; // The legacy controller attachment owns this reference.

    if (fCommandGateAcquired && _fCommandGate) {
        _fCommandGate->disable();
        if (fCommandGateAdded && (!_fWorkloop ||
            _fWorkloop->removeEventSource(_fCommandGate) != kIOReturnSuccess)) return false;
        OSSafeReleaseNULL(_fCommandGate);
    }
    fCommandGateAdded = false;
    fCommandGateAcquired = false;
    OSSafeReleaseNULL(fHalService);
    OSSafeReleaseNULL(fWatchdogWorkLoop);
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    if (driverSnapshotsPipe && fSnapshotsPipeStarted)
        driverSnapshotsPipe->stopPipe();
    fSnapshotsPipeStarted = false;
    OSSafeReleaseNULL(driverSnapshotsPipe);
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverLog);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (direct) {
        IOLockLock(fControllerTimerLock);
        const bool exact = fFailedStartCleanup == authority && authority->claimed &&
            authority->stage == FailedStartCleanupFrame::Stage::ParentStopped;
        if (exact) {
            authority->stage = FailedStartCleanupFrame::Stage::Finished;
            authority->status = kIOReturnSuccess; authority->finished = true;
            authority->published = false; fFailedStartCleanup = NULL;
            fStartInProgress = false;
        }
        IOLockUnlock(fControllerTimerLock);
        if (!exact) return false;
    }
#endif
    return true;
}

IOReturn AirportItlwm::nativeChildCleanupReadyGated(OSObject *owner, void *candidate, void *, void *, void *)
{
    auto *that = OSDynamicCast(AirportItlwm, owner);
    auto *child = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(candidate));
    if (!that || !child || child != that->fNetIf || !_fWorkloop || !_fWorkloop->inGate())
        return kIOReturnNotReady;
    if (!child->controllerCleanupReady())
        return kIOReturnNotReady;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::nativeChildFinishCleanupGated(OSObject *owner, void *candidate,
    void *neverAttached, void *, void *)
{
    auto *that = OSDynamicCast(AirportItlwm, owner);
    IOCommandGate *gate = _fCommandGate;
    auto *child = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(candidate));
    if (!that || !child || child != that->fNetIf || !that->fCleanupInProgress ||
        that->fStartInProgress || !that->fWrapperProvider ||
        !that->fCommandGateAdded || !gate || gate != _fCommandGate)
        return kIOReturnNotReady;
    // Use the retained wrapper lease and gate, never the global queue alias as
    // new ownership. cleanup() and its child local already hold positive refs.
    IOWorkLoop *loop = that->fWrapperProvider->getWorkLoop();
    if (!loop || gate->getWorkLoop() != loop || !loop->inGate() || child->getProvider())
        return kIOReturnNotReady;
    const bool wasNeverAttached = !that->fInterfaceAttached &&
        !that->fControllerInterfaceAttached && !that->fChildTerminationRequested;
    if ((neverAttached != NULL) != wasNeverAttached) return kIOReturnBadArgument;
    loop->retain();
    gate->retain();
    const bool complete = child->finishControllerCleanup(that, gate, loop, wasNeverAttached);
    gate->release();
    loop->release();
    return complete ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwm::nativeChildStopCompleteGated(OSObject *owner, void *candidate, void *, void *, void *)
{
    auto *that = OSDynamicCast(AirportItlwm, owner);
    auto *child = OSDynamicCast(AirportItlwmSkywalkInterface, static_cast<OSObject *>(candidate));
    if (!that || !child || child != that->fNetIf || !_fWorkloop || !_fWorkloop->inGate())
        return kIOReturnNotReady;
    // A void child stop can fail its own admission and still be followed by
    // native detach. Topology alone does not prove borrowed references cleared.
    return child->controllerStopComplete() && child->getProvider() == NULL ?
        kIOReturnSuccess : kIOReturnNotReady;
}

#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
static bool airitl_begin_controller_event_frame(AirportItlwm *owner,
                                               AirportItlwm::ControllerTimerFrame *frame)
{
    if (!owner || !frame || !owner->fControllerTimerLock)
        return false;
    IOLockLock(owner->fControllerTimerLock);
    if (owner->fControllerTimerClosing) {
        IOLockUnlock(owner->fControllerTimerLock);
        return false;
    }
    owner->retain();
    frame->thread = current_thread();
    frame->next = owner->fControllerTimerFrames;
    frame->source = NULL;
    frame->rearm = false;
    frame->owner = owner;
    owner->fControllerTimerFrames = frame;
    IOLockUnlock(owner->fControllerTimerLock);
    return true;
}
#endif

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    if (!ic)
        return;

    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
    if (!that)
        return;

#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    struct EventControllerOwner {
        AirportItlwm *owner = NULL;
        ~EventControllerOwner() { if (owner) owner->release(); }
    } eventOwner;
    ControllerTimerFrame eventFrame;
    if (!airitl_begin_controller_event_frame(that, &eventFrame))
        return;
    eventOwner.owner = that;
#endif

    if (that->fStopping || !that->fHalService || that->fHalService->get80211Controller() != ic)
        return;

#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if (!ic->ic_native_controlled)
        return;
#endif

#if __IO80211_TARGET >= __MAC_15_0
    if (ic->ic_native_controlled) {
        if (_fCommandGate)
            _fCommandGate->runAction(nativeEventGated, ic,
                                    reinterpret_cast<void *>(uintptr_t(msgCode)), data);
        return;
    }
#endif

    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0);
            that->updateLQMIfChanged();
            break;
        case IEEE80211_EVT_SCAN_DONE: {
            AirportItlwmSkywalkInterface *station = OSDynamicCast(AirportItlwmSkywalkInterface, interface);
            if (station)
                station->completeScanRequest();
            break;
        }
        case IEEE80211_EVT_STA_DEAUTH:
            interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0);
            that->resetLQM(true);
            break;
        default:
            break;
    }
}

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    ControllerTimerFrame frame;
    if (!beginControllerTimer(&frame, timer, false)) return;
    if (fStopping) { frame.rearm = false; return; }

    if (fHalService) {
        struct ieee80211com *ic = fHalService->get80211Controller();
        if (ic) {
            struct _ifnet *ifp = &ic->ic_ac.ac_if;
            if (ifp->if_watchdog)
                (*ifp->if_watchdog)(ifp);
        }
    }

    updateLQMIfChanged();
#if __IO80211_TARGET >= __MAC_15_0
    AirportItlwmSkywalkInterface *interface = OSDynamicCast(AirportItlwmSkywalkInterface, fNetIf);
    if (interface) {
        interface->pollNativeScan();
        interface->pollNativeJoin();
    }
#endif
}

void AirportItlwm::updateLQMIfChanged()
{
    if (!fNetIf || !fHalService)
        return;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN) {
        resetLQM();
        return;
    }

    struct ieee80211_node *ni = ic->ic_bss;
    if (!ni) {
        resetLQM();
        return;
    }

    UInt64 lqm;
    if (ni->ni_rssi >= 30)
        lqm = 100;
    else if (ni->ni_rssi >= 15)
        lqm = 50;
    else
        lqm = 25;

    if (lqm != fLastReportedLQM) {
        fLastReportedLQM = lqm;
        fNetIf->setLQM(lqm);
    }
}

void AirportItlwm::resetLQM()
{
    resetLQM(false);
}

void AirportItlwm::resetLQM(bool force)
{
    if (!fNetIf || (!force && fLastReportedLQM == 0))
        return;

    fLastReportedLQM = 0;
    fNetIf->setLQM(0);
}

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that) return;
    ControllerTimerFrame frame;
    if (!that->beginControllerTimer(&frame, sender, true)) return;
    if (that->fStopping || !that->fNetIf) return;

    AirportItlwmSkywalkInterface *station = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (station)
        station->completeScanRequest(true);
}

#define ITL_CONTROLLER_CLASS AirportItlwm
#define ITL_CONTROLLER_HAS_SCAN 1
#include <HAL/ItlControllerTimers.inc>
#undef ITL_CONTROLLER_HAS_SCAN
#undef ITL_CONTROLLER_CLASS

#ifdef AIRITL_LINK_DEFER
static void handleDeferredLinkState(thread_call_param_t, thread_call_param_t);
#endif

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    AIRITL_DBG("AirportItlwm::init properties=%p\n", properties);
    fNativeInitComplete = false;
    fNativeStartLifetimeHeld = false;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    fFailedStartCleanup = NULL;
#endif
    fNativeStartPhase = NativeStartPhase::NotAttempted;
    fFirstNativeInitPhase = FirstNativeInitPhase::InProgress;
    bool ret = super::init(properties);
    fNativeInitComplete = ret;
    fFirstNativeInitPhase = ret ? FirstNativeInitPhase::Succeeded : FirstNativeInitPhase::Failed;
    fHalService = NULL;
    pciNub = NULL;
    fControllerStarted = false;
    fWrapperProvider = NULL;
    fControllerOwnerClaimed = false;
    fTimerProbe = NULL;
    fHalAttached = false;
    fHalAttachAttempted = false;
    fChildTerminationRequested = false;
    fStartInProgress = false;
    fInterfaceAttached = false;
    fInterfacePublished = false;
    fControllerInterfaceAttached = false;
    fEthernetAttached = false;
    fCommandGateAcquired = false;
    fCommandGateAdded = false;
    fScanSourceAdded = false;
    fWatchdogTimerAdded = false;
    fCleanupInProgress = false;
    fStopping = false;
    fNativeHooksInstalled = false;
    fNativeBootstrapDispatched = false;
    fNativeResetActive = false;
    bzero(&fNativeReset, sizeof(fNativeReset));
    fNativeAvailabilityDispatched = false;
    fNativeCoreAvailable = false;
    fNativeCoreReason = 0;
    fNativeCoreFlags = 0;
    fPowerCallLock = ret ? IOLockAlloc() : NULL;
    fPowerCallAdmission = fPowerCallStopping = fPowerCallDraining = fPowerCallOwnerHeld = false;
    fPowerCallFrames = NULL;
    fPowerCallOutstanding = 0;
#ifdef AIRITL_LINK_DEFER
    fLinkStateLock = ret ? IOLockAlloc() : NULL;
    fLinkStateCall = fLinkStateLock ? thread_call_allocate(handleDeferredLinkState, this) : NULL;
    fLinkStateWorker = NULL;
    fLinkStateWanted = kIO80211NetworkLinkDown;
    fLinkStateReason = 0;
    fLinkStateOutstanding = 0;
    fLinkStatePending = fLinkStateStopping = false;
    fLinkStateQueued = fLinkStateRunning = false;
    fNativeLinkEpoch = fLinkStateEpoch = 0;
    fNativeLinkLastUp = fNativeLinkPublished = false;
    if (ret && (!fLinkStateLock || !fLinkStateCall))
        IOLog("AirportItlwm::init deferred link-state allocation failed\n");
#endif
    awdlSyncEnable = true;
    power_state = 0;
    fNativePowerSequence = 0;
    pmPowerState = kPowerStateOn;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    fLatestRequestedPowerState = kPowerStateOn;
#endif
    fLastReportedLQM = 0;
    fpNetStats = NULL;
#ifndef AIRITL_ENABLE_BSDIF
    bzero(&fNativeNetStats, sizeof(fNativeNetStats));
#endif
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    fControllerTimerLock = ret ? IOLockAlloc() : NULL;
    fControllerTimerClosing = fControllerTimerDraining = fControllerTimerOwnerHeld = false;
    fControllerTimerFrames = NULL;
    fRetiredWatchdogTimer = fRetiredScanTimer = NULL;
    fWatchdogTimerAdded = fScanSourceAdded = false;
    watchdogTimer = NULL;
    scanSource = NULL;
#ifdef AIRITL_LINK_DEFER
    if (!fLinkStateLock || !fLinkStateCall) return false;
#endif
    return ret && fPowerCallLock && fControllerTimerLock;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    AIRITL_DBG("AirportItlwm::probe entry provider=%p providerClass=%s score=%d\n",
          provider, provider ? provider->getMetaClass()->getClassName() : "NULL", score ? *score : 0);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        AIRITL_DBG("AirportItlwm::probe rejected non-wrapper provider\n");
        return NULL;
    }
    if (fHalService)
        return NULL;
    pciNub = wrapper->pciNub;
    fHalService = wrapper->createHalService();
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        IOLog("AirportItlwm::probe rejected wrapper without PCI/HAL pci=%p hal=%p\n", pciNub, fHalService);
        return NULL;
    }
#if __IO80211_TARGET >= __MAC_15_0
    IOService *baseProbe = super::probe(provider, score);
    if (!baseProbe) {
        IOLog("AirportItlwm::probe rejected by IO80211Controller for Sequoia wrapper provider=%p pci=%p hal=%p score=%d\n",
              provider, pciNub, fHalService, score ? *score : 0);
        return NULL;
    }
    AIRITL_DBG("AirportItlwm::probe accepted by IO80211Controller for Sequoia wrapper provider=%p pci=%p hal=%p baseProbe=%p score=%d\n",
          provider, pciNub, fHalService, baseProbe, score ? *score : 0);
    return baseProbe;
#else
    return super::probe(provider, score);
#endif
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
    
    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
    
    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
    
    CCStreamOptions faultReportOptions = { 0 };
    faultReportOptions.stream_type = 1;
    faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
#if __IO80211_TARGET >= __MAC_15_0
    if (!driverLogPipe || !driverDataPathPipe || !driverSnapshotsPipe || !_fWorkloop)
        return false;
    CCStream *faultStream = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
    if (!faultStream || !faultStream->metaCast("CCDataStream")) {
        OSSafeReleaseNULL(faultStream);
        return false;
    }
    // The raw reporter retains the stream and workloop. The IO80211 wrapper
    // retains the raw reporter and supplies registerCallbacks at the native slot.
    CCFaultReporter *rawReporter = CCFaultReporter::withStreamWorkloop(
        static_cast<CCDataStream *>(faultStream), _fWorkloop);
    faultStream->release();
    if (!rawReporter)
        return false;
    driverFaultReporter = IO80211FaultReporter::allocWithParams(rawReporter);
    rawReporter->release();
    if (!driverFaultReporter || !driverFaultReporter->metaCast("IO80211FaultReporter")) {
        OSSafeReleaseNULL(driverFaultReporter);
        return false;
    }
#else
    driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
#endif
    XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);

    // The controller LOGGER (getLogger()). IO80211ControllerMonitor::initWithControllerAndProvider
    // (called by IO80211Controller::start -> createIOReporters) FAILS if getLogger() returns null —
    // which made super::start return false (dies in createIOReporters, before
    // getLogPipes/getFaultReporter). Apple's reference (AppleBCMWLANCore) returns a CCLogStream here.
    CCStreamOptions logStreamOptions = { 0 };
    logStreamOptions.stream_type = 0;
    logStreamOptions.console_level = 0xFFFFFFFFFFFFFFFF;
    driverLog = CCLogStream::withPipeAndName(driverLogPipe, "DriverLog", &logStreamOptions);
    XYLog("%s driverLogRet %d\n", __FUNCTION__, driverLog != NULL);

    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter && driverLog;
}

bool AirportItlwm::start(IOService *provider)
{
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    if (!fNativeInitComplete || !fControllerTimerLock) return false;
    struct StartOwner {
        AirportItlwm *owner = NULL;
        ~StartOwner() { if (owner) owner->release(); }
    } startOwner;
    struct PreflightOwners {
        ItlHalService *hal = NULL;
        IOService *provider = NULL;
        IOWorkLoop *loop = NULL;
        void releaseAll() {
            auto *h = hal; hal = NULL; if (h) h->release();
            auto *w = loop; loop = NULL; if (w) w->release();
            auto *p = provider; provider = NULL; if (p) p->release();
        }
        ~PreflightOwners() { releaseAll(); }
    } preflight;
    const thread_t startupCaller = current_thread();
    IOWorkLoop *startupLoop = NULL;
    bool directIwxStartup = false;
    IOLockLock(fControllerTimerLock);
    if (fControllerTimerClosing || fStartInProgress || fControllerStarted || fNetIf || fCleanupInProgress) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    directIwxStartup = OSDynamicCast(ItlIwx, fHalService) != NULL;
    if (directIwxStartup) {
        auto *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
        startupLoop = wrapper ? wrapper->getWorkLoop() : NULL;
        if (!wrapper || !startupLoop || startupLoop != _fWorkloop ||
            startupLoop->inGate() || startupLoop->onThread() ||
            fNativeStartPhase != NativeStartPhase::NotAttempted || fNativeStartLifetimeHeld ||
            fWrapperProvider || fControllerOwnerClaimed || fHalAttached || fHalAttachAttempted ||
            fCommandGateAdded || fCommandGateAcquired || fWatchdogWorkLoop ||
            watchdogTimer || scanSource || fRetiredWatchdogTimer || fRetiredScanTimer ||
            fControllerTimerOwnerHeld || fHalService->isTearingDown()) {
            IOLockUnlock(fControllerTimerLock);
            return false;
        }
        preflight.hal = fHalService; preflight.hal->retain();
        preflight.provider = provider; provider->retain();
        preflight.loop = startupLoop; startupLoop->retain();
        IOLockUnlock(fControllerTimerLock);
        if (!directFailedStartCallerReady(provider, startupLoop, startupCaller)) return false;
        IOLockLock(fControllerTimerLock);
        if (fControllerTimerClosing || fStartInProgress || fControllerStarted || fNetIf ||
            fCleanupInProgress || fHalService != preflight.hal || fWrapperProvider ||
            fControllerOwnerClaimed || fNativeStartPhase != NativeStartPhase::NotAttempted ||
            _fWorkloop != startupLoop) {
            IOLockUnlock(fControllerTimerLock);
            return false;
        }
    }
    fStartInProgress = true;
    fStopping = false;
    IOLockUnlock(fControllerTimerLock);
    retain(); startOwner.owner = this;
#else
    if (!fNativeInitComplete || !fControllerTimerLock) return false;
    IOLockLock(fControllerTimerLock);
    if (fControllerTimerClosing || fStartInProgress || fControllerStarted || fNetIf || fCleanupInProgress) {
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fStartInProgress = true;
    fStopping = false;
    IOLockUnlock(fControllerTimerLock);
    struct StartOwner {
        AirportItlwm *owner;
        ~StartOwner() { owner->release(); }
    } startOwner{this};
    retain();
#endif
    // Global gate and task-queue consumers permit only one active controller.
    if (!OSCompareAndSwapPtr(NULL, this, &gActiveController)) {
        IOLockLock(fControllerTimerLock);
        fStartInProgress = false;
        IOLockUnlock(fControllerTimerLock);
        return false;
    }
    fControllerOwnerClaimed = true;
    auto failStart = [this, provider]() -> bool {
        IOLockLock(fControllerTimerLock);
        fStartInProgress = false;
        IOLockUnlock(fControllerTimerLock);
        cleanup(provider);
        return false;
    };
    auto finishStart = [this, provider]() -> bool {
        IOLockLock(fControllerTimerLock);
        const bool closed = fControllerTimerClosing;
        fStartInProgress = false;
        IOLockUnlock(fControllerTimerLock);
        if (closed) { cleanup(provider); return false; }
        return true;
    };
    auto *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper)
        return failStart();
    wrapper->retain();
    fWrapperProvider = wrapper;
    if (!wrapper->getWorkLoop() || wrapper->getWorkLoop() != _fWorkloop)
        return failStart();
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    preflight.releaseAll();
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    AIRITL_DBG("AirportItlwm::start entry provider=%p providerClass=%s pci=%p hal=%p\n",
          provider, provider ? provider->getMetaClass()->getClassName() : "NULL", pciNub, fHalService);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;
    
    UInt8 builtIn = 0;
    // DIAGNOSTIC: test IO80211TimerSource creation with our workqueue (the exact thing that fails
    // inside RangingManager during super::start). _fWorkloop is set by the wrapper before our start.
    if (getProvider()) {
        getProvider()->setProperty("AIRITL_TT_WQ", _fWorkloop ? "set" : "null");
        if (_fWorkloop) {
            IO80211TimerSource *tt = IO80211TimerSource::allocWithParams(
                kIO80211TimerSourcePriorityNormal, this, _fWorkloop, airitl_timer_test_cb);
            getProvider()->setProperty("AIRITL_TIMERTEST", tt ? "OK" : "NULL");
            fTimerProbe = reinterpret_cast<OSObject *>(tt);
        }
    }
    OSData *builtInData = OSData::withBytes(&builtIn, sizeof(builtIn));
    if (builtInData) {
        setProperty("built-in", builtInData);
        builtInData->release();
    }
    setProperty("DriverKitDriver", kOSBooleanFalse);
    // CC logs + driverFaultReporter MUST exist BEFORE super::start(): Apple's IO80211Controller::start
    // -> findAndAttachToFaultReporter()/setupControlPathLogging() call our getFaultReporterFromDriver()/
    // logger accessors and FAIL (start returns false) if they are null. The reference driver
    // AppleBCMWLANCore::start creates its logger/fault reporter before calling super::start too.
    // (Was previously called ~150 lines later, which made super::start fail at boot.)
    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        IOLog("AirportItlwm::start CCLog init failed (pre-super)\n");
        return failStart();
    }
    g_airitlSelf = this;
    g_airitlInSuperStart = true;
    // Failed native start keeps this positive reference and its dependency graph.
    retain();
    fNativeStartLifetimeHeld = true;
    fNativeStartPhase = NativeStartPhase::InProgress;
    bool __superRet = super::start(provider);
    fNativeStartPhase = __superRet ? NativeStartPhase::Succeeded : NativeStartPhase::Failed;
    if (__superRet) {
        fNativeStartLifetimeHeld = false;
        release(); // StartOwner still holds the controller through this call site.
    }
    g_airitlInSuperStart = false;
    // record super::start's actual return on the provider (survives our detach) for post-boot diagnosis
    if (getProvider()) getProvider()->setProperty("AIRITL_SUPERSTART", __superRet ? "true" : "false");
    if (!__superRet) {
        IOLog("AirportItlwm::start super::start failed\n");
        return failStart();
    }
    fControllerStarted = true;
    if (!pciNub || !fHalService) {
        XYLog("missing PCI nub or HAL service\n");
        IOLog("AirportItlwm::start missing PCI nub or HAL pci=%p hal=%p\n", pciNub, fHalService);

        return failStart();
    }

    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        IOLog("AirportItlwm::start requestPowerDomainState failed\n");
        return failStart();
    }
    if (initPCIPowerManagment(pciNub) == false) {
        IOLog("AirportItlwm::start initPCIPowerManagement failed\n");
        return failStart();
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        IOLog("AirportItlwm::start missing shared workloop\n");
        return failStart();
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        IOLog("AirportItlwm::start command gate allocation failed\n");
        return failStart();
    }
    fCommandGateAcquired = true;
    if (_fWorkloop->addEventSource(_fCommandGate) != kIOReturnSuccess) {
        XYLog("add command gate fail\n");
        IOLog("AirportItlwm::start add command gate failed\n");
        return failStart();
    }
    fCommandGateAdded = true;
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        IOLog("AirportItlwm::start setup medium failed\n");
        return failStart();
    }
    if (!fHalService->initWithController(this, _fWorkloop, _fCommandGate)) {
        IOLog("AirportItlwm::start HAL controller initialization failed\n");
        return failStart();
    }
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic) {
        XYLog("missing 80211 controller\n");
        IOLog("AirportItlwm::start missing 80211 controller\n");
        return failStart();
    }
#ifndef AIRITL_ENABLE_BSDIF
    // HAL RX/TX paths update these counters without a legacy network interface.
    // Keep the backing storage alive through HAL detach and callback draining.
    fpNetStats = &fNativeNetStats;
    ic->ic_ac.ac_if.netStat = fpNetStats;
#endif
    ic->ic_event_handler = eventHandler;
    
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        ic->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        ic->ic_userflags |= IEEE80211_F_NOHT40;
    airitl_debug_log = PE_parse_boot_argn("-airitldbg", &boot_value, sizeof(boot_value)) ? 1 : 0;
    
    fHalAttachAttempted = true;
    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
        IOLog("AirportItlwm::start HAL attach failed\n");
        return failStart();
    }
    fHalAttached = true;
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    // Native requests, packet queues, HAL events and the nonblocking Iwx
    // watchdog share one gate. Avoid cross-workloop waits during teardown.
    fWatchdogWorkLoop = _fWorkloop;
    fWatchdogWorkLoop->retain();
#else
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
#endif
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        IOLog("AirportItlwm::start watchdog workloop allocation failed\n");
        return failStart();
    }
    if (!holdControllerTimerOwner()) return failStart();
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        IOLog("AirportItlwm::start watchdog timer allocation failed\n");
        return failStart();
    }
    if (fWatchdogWorkLoop->addEventSource(watchdogTimer) != kIOReturnSuccess) {
        XYLog("add watchdog timer fail\n");
        IOLog("AirportItlwm::start add watchdog timer failed\n");
        return failStart();
    }
    fWatchdogTimerAdded = true;
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
    if (!scanSource) {
        XYLog("init scan timer fail\n");
        IOLog("AirportItlwm::start scan timer allocation failed\n");
        return failStart();
    }
    if (_fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        XYLog("add scan timer fail\n");
        IOLog("AirportItlwm::start add scan timer failed\n");
        return failStart();
    }
    fScanSourceAdded = true;
    scanSource->enable();

    AirportItlwmSkywalkInterface *newInterface = new AirportItlwmSkywalkInterface;
    fNetIf = newInterface;
    if (!fNetIf) {
        XYLog("Skywalk interface alloc fail\n");
        IOLog("AirportItlwm::start Skywalk interface allocation failed\n");
        return failStart();
    }
    if (!newInterface->init(this)) {
        XYLog("Skywalk interface init fail\n");
        IOLog("AirportItlwm::start Skywalk interface init failed\n");
        if (newInterface->fFirstNativeInitPhase == AirportItlwmSkywalkInterface::FirstNativeInitPhase::Failed &&
            !newInterface->fNativeInitComplete && fNetIf == newInterface) {
            fNetIf = NULL;
            newInterface->release();
        }
#if __IO80211_TARGET >= __MAC_15_0
        else if (fNetIf == newInterface &&
                 newInterface->prepareFailedSourceInitDisposal(this) && fNetIf == newInterface) {
            AirportItlwmSkywalkInterface *failedInterface = newInterface;
            fNetIf = NULL;
            failedInterface->release();
        }
#endif
        return failStart();
    }
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);

    // initCCLogs() moved earlier (before super::start) — see note there. Keep a guard in case it
    // somehow wasn't initialized.
    if (!driverFaultReporter && !initCCLogs()) {
        XYLog("CCLog init fail\n");
        IOLog("AirportItlwm::start CCLog init failed\n");
        return failStart();
    }
#if __IO80211_TARGET >= __MAC_15_0
    // corecapture starts a pipe (CCPipe::startPipe -> CCDataPipe::start, which
    // creates the pipe's notify timer) only from its CoreCapture plane, which
    // this system does not have. Without that timer the first IO80211 fault
    // report (link loss, driver reset) faults in CCDataPipe::enqueueBlob on a
    // NULL timer. Start the snapshot pipe here, after super::start() gave us
    // a work loop; startPipe() is idempotent under the pipe's own lock.
    if (driverSnapshotsPipe && !fSnapshotsPipeStarted && getWorkLoop()) {
        fSnapshotsPipeStarted = driverSnapshotsPipe->startPipe();
        XYLog("%s: snapshot pipe startPipe -> %d\n", __FUNCTION__, fSnapshotsPipeStarted);
    }
#endif
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        IOLog("AirportItlwm::start Skywalk attach to service failed\n");
        return failStart();
    }
    fInterfaceAttached = true;
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        IOLog("AirportItlwm::start attachInterface failed\n");
        return failStart();
    }
    fControllerInterfaceAttached = true;
    // The legacy BSD shim remains an explicit diagnostic option.
#ifdef AIRITL_ENABLE_BSDIF
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface, true)) {
        XYLog("attach to IONetworkController interface fail\n");
        IOLog("AirportItlwm::start IONetworkController attachInterface failed\n");
        return failStart();
    }
#else
    AIRITL_DBG("AirportItlwm::start skipping legacy bsdInterface dual-attach (stable baseline)\n");
#endif
    memset(&registInfo, 0, sizeof(registInfo));
    // Host getter audit: this initializer handles absent private records.
    // Framework registration below owns their allocation and copies.
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        IOLog("AirportItlwm::start initRegistrationInfo failed\n");
        return failStart();
    }
    IOEthernetAddress hardwareAddress = {};
    if (getHardwareAddress(&hardwareAddress) != kIOReturnSuccess) {
        IOLog("AirportItlwm::start hardware address unavailable\n");
        return failStart();
    }
    ether_addr macAddress = {};
    static_assert(sizeof(macAddress.octet) == sizeof(hardwareAddress.bytes), "MAC size mismatch");
    memcpy(macAddress.octet, hardwareAddress.bytes, sizeof(macAddress.octet));
    registInfo.setHardwareAddress(macAddress);
#if __IO80211_TARGET >= __MAC_15_0
    registInfo.setWiFiDiscoveryIdentity();
#endif
    if (fNetIf->getInterfaceRole() == 1) {
#if __IO80211_TARGET >= __MAC_15_0
        if (fNetIf->deferBSDAttach(true) != kIOReturnSuccess)
            return failStart();
#else
        fNetIf->deferBSDAttach(true);
#endif
    }
#ifdef AIRITL_DATAPATH_P0
    // Provision a minimal Skywalk datapath (real-but-tiny pool + 4 dummy queues)
    // BEFORE fNetIf->start(), so the datapath getters return valid objects instead of the inherited
    // null base stubs that the framework derefs during airportd's datapath enable (the proven fault).
    AirportItlwmSkywalkInterface *p0If = OSDynamicCast(AirportItlwmSkywalkInterface, fNetIf);
    // Hand the interface the iwx device's DMA mapper so the pool's segments become DMA-capable.
    // copyMapperForDevice returns a +1 ref; provisionDatapath/the pool retains it, so release ours after.
    IOMapper *dmaMapper = IOMapper::copyMapperForDevice(pciNub);   // pciNub is the real IOPCIDevice; the provider is the synthetic IOPCIEDeviceWrapper (not in the VT-d registry -> null mapper)
    bool provisioned = (p0If && p0If->provisionDatapath(dmaMapper));
    if (dmaMapper)
        dmaMapper->release();
    if (!provisioned) {
        XYLog("datapath provision fail\n");
        IOLog("AirportItlwm::start datapath provisioning failed\n");
        return failStart();
    }
#endif
    if (!fNetIf->start(this)) {
        XYLog("Skywalk interface start fail\n");
        IOLog("AirportItlwm::start Skywalk interface start failed\n");
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
        const bool directAbort = directIwxStartup &&
            newInterface->fSourceInitPhase == AirportItlwmSkywalkInterface::SourceInitPhase::Succeeded &&
            newInterface->fNativeStartPhase == AirportItlwmSkywalkInterface::NativeStartPhase::Failed;
        if (directAbort && !cleanupFailedNativeStart(provider, newInterface, startupLoop, startupCaller))
            IOLog("AirportItlwm::start direct cleanup remains unresolved\n");
        if (directAbort) return false;
#endif
        return failStart();
    }

#ifdef AIRITL_DP_REGISTER
    // The native infra driver registers after successful interface start.
    AirportItlwmSkywalkInterface *registeredInterface = OSDynamicCast(AirportItlwmSkywalkInterface, fNetIf);
    if (!registeredInterface) {
        IOLog("AirportItlwm::start registration interface cast failed\n");
        return failStart();
    }
    IOReturn registrationResult = registeredInterface->registerDatapath(&registInfo);
    IOLog("AirportItlwm::start registerDatapath -> 0x%x\n", registrationResult);
    if (registrationResult != kIOReturnSuccess)
        return failStart();
#endif

#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if (!_fCommandGate ||
        _fCommandGate->runAction(nativeHooksGated, (void *)1) != kIOReturnSuccess)
        return failStart();
    // HAL attach has completed firmware preinitialization and populated the
    // hardware MAC and channels. Notify both native consumers before matching.
    if (_fCommandGate->runAction(nativeBootstrapGated) != kIOReturnSuccess)
        return failStart();
#endif
    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&ic->ic_ess))
        ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
    // The framework never calls enable() on this target, so bring the radio up
    // here. Non fatal: publish still proceeds and the log names a refusal.
    power_state = 1;
    changePowerStateToPriv(kPowerStateOn);
    {
        const IOReturn radio = enableAdapter(NULL);
        if (radio != kIOReturnSuccess)
            IOLog("AirportItlwm::start radio enable refused 0x%x\n", radio);
    }
#ifdef AIRITL_DP_REGISTER
    // Interface matching starts only after registration and final state setup.
    fInterfacePublished = true;
#if __IO80211_TARGET >= __MAC_15_0
    // The native method removes IODeferBSDAttach and registers the service.
    // Keep ownership marked before matching can start the BSD client.
    fNetIf->registerService(kIOServiceAsynchronous);
    if (fNetIf->deferBSDAttach(false) != kIOReturnSuccess)
        return failStart();
    // Apple's own driver announces readiness with this interface property
    // (AppleBCMWLANCore::signalDriverReady). CoreWiFi watches it; without it
    // airportd never starts issuing setPOWER or any other command.
    {
        OSString *ready = OSString::withCString("TRUE");
        if (ready) {
            fNetIf->setProperty("CoreWiFiDriverReadyKey", ready);
            setProperty("CoreWiFiDriverReadyKey", ready);
            ready->release();
        } else {
            IOLog("AirportItlwm::start driver-ready property alloc failed\n");
        }
    }
#else
    fNetIf->registerService();
#endif
#endif
    registerService();
    // Arm the watchdog here as well: enable() may arrive later or bail, and
    // without the tick nothing polls native joins or sees a lost beacon.
    if (!armControllerWatchdog())
        IOLog("AirportItlwm::start watchdog arm failed\n");
    AIRITL_DBG("AirportItlwm::start succeeded\n");
    return finishStart();
}

void AirportItlwm::stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    cleanup(provider);
}

void AirportItlwm::free()
{
    if (fFirstNativeInitPhase == FirstNativeInitPhase::Failed && !fNativeInitComplete) {
        super::free();
        return;
    }
    if (!fNativeInitComplete) return;
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (!cleanup(getProvider(), true)) return;
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
#ifdef AIRITL_LINK_DEFER
    if (fLinkStateCall && !thread_call_isactive(fLinkStateCall) && thread_call_free(fLinkStateCall))
        fLinkStateCall = NULL;
    if (fLinkStateLock && !fLinkStateCall) {
        IOLockFree(fLinkStateLock);
        fLinkStateLock = NULL;
    }
#endif
    if (fControllerTimerLock) {
        IOLockFree(fControllerTimerLock);
        fControllerTimerLock = NULL;
    }
    IOPCIEDeviceWrapper *wrapperProvider = fWrapperProvider;
    fWrapperProvider = NULL;
    AirportItlwm *controllerOwner = fControllerOwnerClaimed ? this : NULL;
    fControllerOwnerClaimed = false;
    super::free();
    if (controllerOwner)
        OSCompareAndSwapPtr(controllerOwner, NULL, &gActiveController);
    if (wrapperProvider)
        wrapperProvider->release();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    airitl_ss_hit("AIRITL_SS_CREATEWQ");
    return _fWorkloop != 0;
}

#if __IO80211_TARGET >= __MAC_15_0
IO80211WorkQueue *AirportItlwm::getWorkQueue() const
#else
IO80211WorkQueue *AirportItlwm::getWorkQueue()
#endif
{
    // diagnostic: getWorkQueue is called by Apple's IO80211Controller::start from MULTIPLE sites
    // (CommandGate alloc, CreatePostOffice, calm-timer, TimerFactory). Capture the caller's return
    // address (offset into IO80211Family) to pinpoint WHICH site fired last before super::start failed.
    static unsigned long long __wqn = 0;
    __wqn++;
    void *ret = __builtin_return_address(0);
    if (getProvider()) {
        getProvider()->setProperty("AIRITL_WQN", __wqn, 32);
        getProvider()->setProperty("AIRITL_WQPTR", _fWorkloop ? "valid" : "null");
        if (g_airitlInSuperStart) getProvider()->setProperty("AIRITL_SS_WORKQUEUE", "yes");
        // absolute slid return address (resolve in lldb / against kc load addr)
        getProvider()->setProperty("AIRITL_WQCALLER", (unsigned long long)(uintptr_t)ret, 64);
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)ret);
        getProvider()->setProperty("AIRITL_WQCALLER_HEX", buf);
    }
    return _fWorkloop;
}

void *AirportItlwm::getFaultReporterFromDriver()
{
    if (getProvider()) getProvider()->setProperty("AIRITL_FAULTREP", driverFaultReporter ? "set" : "null");
    airitl_ss_hit("AIRITL_SS_FAULTREP");
    return driverFaultReporter;
}

IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (!netif)
        return kIOReturnBadArgument;

    IOReturn ret = super::enable(netif);
    if (ret != kIOReturnSuccess)
        return ret;

    if (_fCommandGate)
        _fCommandGate->enable();
    if (power_state) {
        ret = enableAdapter(bsdInterface);
        if (ret != kIOReturnSuccess)
            return ret;
    }
    // The watchdog is the only thing that polls native joins and notices lost
    // beacons. Nothing armed it on the Skywalk path, so neither happened.
    if (!armControllerWatchdog())
        IOLog("AirportItlwm::enable watchdog arm failed\n");
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    if (!fHalService)
        return false;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return false;

    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    
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
    fEthernetAttached = true;
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    if (fHalService->getDriverInfo())
        netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    if (!fNetIf)
        return NULL;

    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this, fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;
    if (primary)
        *primary = NULL;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    if (!medium || !IONetworkMedium::addMedium(mediumDict, medium)) {
        if (medium) medium->release();
        mediumDict->release();
        return false;
    }
    const IONetworkMedium *primaryMedium = medium;
    medium->release();
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    if (!medium || !IONetworkMedium::addMedium(mediumDict, medium)) {
        if (medium) medium->release();
        mediumDict->release();
        return false;
    }
    medium->release();
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }
    if (result && primary)
        *primary = primaryMedium;

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    if (!fHalService)
        return false;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return false;

    struct _ifnet *ifq = &ic->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            if (bsdInterface)
                bsdInterface->startOutputThread();
#endif
#ifdef AIRITL_LINK_DEFER
            if (!requestLinkState(kIO80211NetworkLinkUp, 0))
                ret = false;
#else
            IOCommandGate *gate = getCommandGate();
            if (gate) {
                IOReturn gateRet = gate->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
                if (gateRet != kIOReturnSuccess) {
                    XYLog("%s link-up state dispatch failed: 0x%x\n", __FUNCTION__, gateRet);
                    ret = false;
                }
            } else {
                XYLog("%s missing command gate for link-up state dispatch\n", __FUNCTION__);
                ret = false;
            }
#endif
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            if (bsdInterface) {
                bsdInterface->stopOutputThread();
                bsdInterface->flushOutputQueue();
            }
#endif
            if (ifq->if_snd.queue)
                ifq_flush(&ifq->if_snd);
            mq_purge(&ic->ic_mgtq);
#ifdef AIRITL_LINK_DEFER
            if (!requestLinkState(kIO80211NetworkLinkDown, (UInt32)ic->ic_deauth_reason))
                ret = false;
#else
            IOCommandGate *gate = getCommandGate();
            if (gate) {
                IOReturn gateRet = gate->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)(uintptr_t)ic->ic_deauth_reason);
                if (gateRet != kIOReturnSuccess) {
                    XYLog("%s link-down state dispatch failed: 0x%x\n", __FUNCTION__, gateRet);
                    ret = false;
                }
            } else {
                XYLog("%s missing command gate for link-down state dispatch\n", __FUNCTION__);
                ret = false;
            }
#endif
        }
    }
    return ret;
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    if (!that || !that->fNetIf)
        return kIOReturnNotAttached;

#if __IO80211_TARGET >= __MAC_15_0
    IOCommandGate *gate = that->getCommandGate();
    IOWorkLoop *loop = gate ? gate->getWorkLoop() : nullptr;
    if (!gate || !loop || loop != _fWorkloop || !loop->inGate() || loop->onThread()) {
        XYLog("%s invalid native link-call context\n", __FUNCTION__);
        return kIOReturnNotPermitted;
    }
    const IO80211LinkState state = (IO80211LinkState)(uintptr_t)arg0;
    const UInt32 reason = (UInt32)(uintptr_t)arg1;
#ifndef AIRITL_ENABLE_BSDIF
    if (state == kIO80211NetworkLinkDown) {
        AirportItlwmSkywalkInterface *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
        if (!interface)
            return kIOReturnNotAttached;
        interface->setCurrentApAddress(nullptr);
    }
#endif
    const bool changed = that->fNetIf->setLinkState(state, 0,
        state == kIO80211NetworkLinkDown && reason != 0, reason);
#else
    const bool changed = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
#endif
    if (!changed) {
        XYLog("%s setLinkState failed\n", __FUNCTION__);
        return kIOReturnError;
    }
#ifdef AIRITL_LINK_DEFER
    // The native setter and the synchronous postMessage callouts can yield
    // main while they wait on serial. If a native publication ran meanwhile,
    // it is newer: stop this legacy tail at that point.
    const UInt32 *epoch = static_cast<const UInt32 *>(arg2);
    const IO80211LinkState tailState = (IO80211LinkState)(uint64_t)arg0;
    auto superseded = [that, epoch, tailState](const char *where) -> bool {
        if (!epoch || !that->nativeLinkSupersedes(*epoch, tailState)) return false;
        AIRITL_DBG("setLinkStateGated legacy link tail skipped after %s: native publication\n", where);
        return true;
    };
#define AIRITL_TAIL_CHECK(where) do { if (superseded(where)) return kIOReturnAborted; } while (0)
#else
#define AIRITL_TAIL_CHECK(where) do { } while (0)
#endif
    AIRITL_TAIL_CHECK("setter");

    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
    AIRITL_TAIL_CHECK("LINK_CHANGED");
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
    AIRITL_TAIL_CHECK("BSSID_CHANGED");
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
    AIRITL_TAIL_CHECK("SSID_CHANGED");
#undef AIRITL_TAIL_CHECK
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
        that->updateLQMIfChanged();
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
        that->resetLQM(true);
    }
    if (that->bsdInterface)
        that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return kIOReturnSuccess;
}

#ifdef AIRITL_LINK_DEFER
static void handleDeferredLinkState(thread_call_param_t param0, thread_call_param_t)
{
    static_cast<AirportItlwm *>(param0)->runDeferredLinkState();
}

// In the main gate: drop a legacy state that a native publication has
// superseded, and an UP whose HAL association is gone. Otherwise publish it,
// with setLinkStateGated re-checking the epoch after the native setter.
IOReturn AirportItlwm::deferredLinkGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    const UInt32 *epoch = static_cast<const UInt32 *>(arg2);
    if (!that || !epoch) return kIOReturnNotAttached;
    if (that->nativeLinkSupersedes(*epoch, (IO80211LinkState)(uintptr_t)arg0)) {
        AIRITL_DBG("%s legacy link state superseded by native publication\n", __FUNCTION__);
        return kIOReturnAborted;
    }
    if ((IO80211LinkState)(uintptr_t)arg0 == kIO80211NetworkLinkUp) {
        struct ieee80211com *ic = that->fHalService ? that->fHalService->get80211Controller() : NULL;
        if (!ic || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss) {
            XYLog("%s legacy link up dropped: HAL no longer associated\n", __FUNCTION__);
            return kIOReturnAborted;
        }
    }
    return setLinkStateGated(target, arg0, arg1, arg2, NULL);
}

bool AirportItlwm::requestLinkState(IO80211LinkState state, UInt32 reason)
{
    if (!fLinkStateLock) return false;
    IOLockLock(fLinkStateLock);
    if (fLinkStateStopping || !fLinkStateCall) {
        IOLockUnlock(fLinkStateLock);
        XYLog("%s deferred link %s refused: stopping\n", __FUNCTION__,
              state == kIO80211NetworkLinkUp ? "up" : "down");
        return false;
    }
    fLinkStateWanted = state;
    fLinkStateReason = reason;
    // Callers run in the main gate (RX/legacy link path), where the native
    // epoch is written, so this read orders the request against native ones.
    fLinkStateEpoch = fNativeLinkEpoch;
    fLinkStatePending = true;
    // Serial publisher: a queued or running worker re-reads the mailbox
    // under this lock before it exits, so it picks up this state.
    if (!fLinkStateQueued && !fLinkStateRunning) {
        retain();
        if (thread_call_enter(fLinkStateCall)) release();
        else ++fLinkStateOutstanding;
        fLinkStateQueued = true;
    }
    IOLockUnlock(fLinkStateLock);
    return true;
}

// Runs on a thread-call thread: enters the main gate from outside the main
// workloop thread, which is the context the native setter admits.
// No driver lock is held across runAction; the native setter may wait.
void AirportItlwm::runDeferredLinkState()
{
    IOLockLock(fLinkStateLock);
    fLinkStateQueued = false;
    fLinkStateRunning = true;
    fLinkStateWorker = current_thread();
    for (;;) {
        if (!fLinkStatePending || fLinkStateStopping) break;
        const IO80211LinkState state = fLinkStateWanted;
        const UInt32 reason = fLinkStateReason;
        UInt32 epoch = fLinkStateEpoch;
        fLinkStatePending = false;
        IOLockUnlock(fLinkStateLock);
        IOCommandGate *gate = getCommandGate();
        IOReturn result = gate ?
            gate->runAction(deferredLinkGated, (void *)(uintptr_t)state, (void *)(uintptr_t)reason, &epoch) :
            kIOReturnNotReady;
        XYLog("%s deferred link %s -> 0x%x\n", __FUNCTION__,
              state == kIO80211NetworkLinkUp ? "up" : "down", result);
        IOLockLock(fLinkStateLock);
    }
    fLinkStateWorker = NULL;
    fLinkStateRunning = false;
    --fLinkStateOutstanding;
    IOLockUnlock(fLinkStateLock);
    release();
}

// Stops new requests and waits until no deferred publication is queued or
// running, so no publication can reach a dismantled interface.
bool AirportItlwm::quiesceLinkStateCall()
{
    if (!fLinkStateLock) return true;
    IOLockLock(fLinkStateLock);
    fLinkStateStopping = true;
    fLinkStatePending = false;
    if (fLinkStateWorker == current_thread()) {
        IOLockUnlock(fLinkStateLock);
        return false;
    }
    thread_call_t call = fLinkStateCall;
    IOLockUnlock(fLinkStateLock);
    if (!call) return true;
    IOLockLock(fLinkStateLock);
    const bool cancelled = thread_call_cancel(call);
    if (cancelled) {
        --fLinkStateOutstanding;
        fLinkStateQueued = false;
    }
    IOLockUnlock(fLinkStateLock);
    if (cancelled) release();
    for (;;) {
        IOLockLock(fLinkStateLock);
        const bool busy = fLinkStateOutstanding != 0;
        IOLockUnlock(fLinkStateLock);
        if (!busy && !thread_call_isactive(call)) break;
        IOCommandGate *gate = getCommandGate();
        if (gate && gate->getWorkLoop() && gate->getWorkLoop()->inGate()) {
            AbsoluteTime deadline;
            clock_interval_to_deadline(10, kMillisecondScale,
                reinterpret_cast<uint64_t *>(&deadline));
            gate->commandSleep(&fLinkStateOutstanding, deadline, THREAD_UNINT);
        } else IOSleep(10);
    }
    return true;
}
#endif

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    if (!interface || !fHalService)
        return kIOReturnNotAttached;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnNotAttached;

    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    if (!ifp->if_snd.queue)
        return kIOReturnNoResources;

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

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

extern const char* hexdump(uint8_t *buf, size_t len);

bool AirportItlwm::nativePacketOutputReady() const
{
    if (fStopping || !fHalService)
        return false;
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || ic->ic_state != IEEE80211_S_RUN ||
        !fHalService->nativeDataOutputReady())
        return false;
    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    return (ifp->if_flags & IFF_RUNNING) && ifp->if_snd.queue &&
        !ifq_is_oactive(&ifp->if_snd) &&
        ifp->if_snd.queue->getSize() < ifp->if_snd.queue->getCapacity();
}

void AirportItlwm::logNativeOutputState(const char *why) const
{
    // Read-only snapshot of each nativePacketOutputReady gate.
    struct ieee80211com *ic = fHalService ? fHalService->get80211Controller() : NULL;
    if (!ic) {
        IOLog("AirportItlwm: tx gate %s: stopping=%d hal=%d ic=0\n", why, fStopping, fHalService != NULL);
        return;
    }
    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    IOLog("AirportItlwm: tx gate %s: stopping=%d state=%d halReady=%d running=%d mgmtOnly=%d "
          "snd=%d oactive=%d len=%u cap=%u\n", why, fStopping, (int)ic->ic_state,
          fHalService->nativeDataOutputReady(), (ifp->if_flags & IFF_RUNNING) != 0,
          (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY) != 0, ifp->if_snd.queue != NULL,
          ifp->if_snd.queue ? (int)ifq_is_oactive(&ifp->if_snd) : -1,
          ifp->if_snd.queue ? (unsigned)ifp->if_snd.queue->getSize() : 0,
          ifp->if_snd.queue ? (unsigned)ifp->if_snd.queue->getCapacity() : 0);
}

UInt32 AirportItlwm::submitNativePacket(mbuf_t packet)
{
    // This helper consumes the mbuf on every return, including a changed state.
    if (!nativePacketOutputReady()) {
        if (packet)
            freePacket(packet);
        return kIOReturnOutputDropped;
    }
    return outputPacket(packet, NULL);
}

UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    if (fStopping || !fHalService) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }

    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    
    if (ic->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL ||
        (ic->ic_native_controlled && !fHalService->nativeDataOutputReady())) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        if (ifp->netStat)
            ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        if (ifp->netStat)
            ifp->netStat->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        if (ifp->netStat)
            ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    size_t len = mbuf_len(m);
    ether_header_t *eh = (ether_header_t *)mbuf_data(m);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(m), len);
        XYLog("output EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (ifp->if_start)
        (*ifp->if_start)(ifp);
    return ret;
}

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    if (!fHalService || !fHalService->getDriverInfo())
        return OSString::withCString("Intel Wi-Fi");
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (!addrP || !fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || IEEE80211_ADDR_EQ(etheranyaddr, ic->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, ic->ic_myaddr);
        return kIOReturnSuccess;
    }
}

#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwm::getHardwareAddressForInterface(IOEthernetAddress *addrP)
{
    return getHardwareAddress(addrP);
}

IOReturn AirportItlwm::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!addrP)
        return kIOReturnError;

    return setHardwareAddress(addrP->bytes, sizeof(addrP->bytes));
}
#endif

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP || addrBytes < IEEE80211_ADDR_LEN || !fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    if_setlladdr(&ic->ic_ac.ac_if, (const UInt8 *)addrP);
    if (ic->ic_state > IEEE80211_S_INIT) {
#ifdef AIRITL_ENABLE_BSDIF
        if (!bsdInterface)
            return kIOReturnNotAttached;
#endif

        IOReturn ret = fHalService->disable(bsdInterface);
        if (ret != kIOReturnSuccess)
            return ret;

        ret = enableAdapter(bsdInterface);
        if (ret != kIOReturnSuccess)
            return ret;
    }
    return kIOReturnSuccess;
}

UInt32 AirportItlwm::getFeatures() const
{
    if (!fHalService || !fHalService->getDriverInfo())
        return 0;
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
    if (!fHalService || !fHalService->getDriverController())
        return kIOReturnNotAttached;
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (!filters)
        return kIOReturnBadArgument;

    *filters = 0;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    airitl_ss_hit("AIRITL_SS_LOGPIPES");
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    if (!cd)
        return kIOReturnError;

    memset(cd, 0, sizeof(struct apple80211_capability_data));
    cd->version = APPLE80211_VERSION;

    if (!fHalService)
        return kIOReturnError;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnError;

    uint32_t caps = ic->ic_caps;
    
#if __IO80211_TARGET < __MAC_15_0
    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
#endif
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards
    
    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

#if __IO80211_TARGET >= __MAC_15_0
    // Advertise named, implemented capabilities. The historical opaque masks
    // claimed native features whose protocols this adapter does not implement.
    if (caps & IEEE80211_C_QOS)
        cd->capabilities[2] |= 1 << (APPLE80211_CAP_WME - 16);
    if (ic->ic_htcaps & IEEE80211_HTCAP_SGI20)
        cd->capabilities[2] |= 1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16);
    if (ic->ic_htcaps & IEEE80211_HTCAP_SGI40)
        cd->capabilities[2] |= 1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16);
    // Observed Apple 24G720 Apple80211Scan waits for scan-completion events with this bit.
    cd->capabilities[5] |= 0x40;
#else
    cd->capabilities[2] = 0xFF & ~(1 << (APPLE80211_CAP_WOW - 16)); // WOW Apple80211 methods are unsupported in this path.
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
#endif
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);
    
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    if (!fHalService)
        return kIOReturnNotAttached;

    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnNotAttached;

    const char *fwVersion = driverInfo->getFirmwareVersion();
    if (!fwVersion)
        return kIOReturnError;

    memset(hv, 0, sizeof(*hv));
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fwVersion);
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    if (!fHalService)
        return kIOReturnNotAttached;

    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnNotAttached;

    const char *fwVersion = driverInfo->getFirmwareVersion();
    if (!fwVersion)
        return kIOReturnError;

    memset(hv, 0, sizeof(*hv));
    hv->version = APPLE80211_VERSION;
    strlcpy(hv->string, fwVersion, sizeof(hv->string));
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

#if __IO80211_TARGET >= __MAC_15_0
static IOReturn airitlNativeCountryGated(OSObject *owner, void *argument, void *setting, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwm, owner);
    auto *data = static_cast<apple80211_country_code_data *>(argument);
    if (!self || !data)
        return kIOReturnBadArgument;
    if (self->fStopping || !self->fHalService)
        return kIOReturnNotAttached;
    if (!self->fHalService->nativeRegulatoryReady())
        return kIOReturnNotReady;
    auto *info = self->fHalService->getDriverInfo();
    const char *country = info ? info->getFirmwareCountryCode() : NULL;
    if (!country || !country[0] || !country[1])
        return kIOReturnNotReady;
    apple80211_country_code_data result = {};
    result.version = APPLE80211_VERSION;
    memcpy(result.cc, country, sizeof(result.cc));
    if (setting) {
        // The native setter does not validate the version word. A successful
        // different-country request needs a real backend operation, not a hint.
        return !memcmp(data->cc, result.cc, sizeof(result.cc)) ?
            kIOReturnSuccess : kIOReturnUnsupported;
    }
    memcpy(data, &result, sizeof(result));
    return kIOReturnSuccess;
}
#endif

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!cd)
        return kIOReturnBadArgument;
    if (!_fCommandGate || !fCommandGateAdded)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(airitlNativeCountryGated, cd);
#else
    char user_override_cc[3];
    if (!cd || !fHalService)
        return kIOReturnError;

    ItlDriverInfo *driverInfo = fHalService->getDriverInfo();
    if (!driverInfo)
        return kIOReturnError;

    const char *cc_fw = driverInfo->getFirmwareCountryCode();
    if (!cc_fw)
        return kIOReturnError;

    memset(cd, 0, sizeof(*cd));
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate || !fCommandGateAdded)
        return kIOReturnNotReady;
    apple80211_country_code_data request;
    memcpy(&request, data, sizeof(request));
    return _fCommandGate->runAction(airitlNativeCountryGated, &request, this);
#else
    if (!data)
        return kIOReturnError;

    char cc[APPLE80211_MAX_CC_LEN + 1];
    memcpy(cc, data->cc, APPLE80211_MAX_CC_LEN);
    cc[APPLE80211_MAX_CC_LEN] = '\0';

    XYLog("%s cc=%s\n", __FUNCTION__, cc);
    if (data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        if (fNetIf)
            fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
    }
    return kIOReturnSuccess;
#endif
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    memset(pd, 0, sizeof(*pd));
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if (!_fCommandGate || !fCommandGateAdded)
        return kIOReturnNotReady;
    auto request = *pd;
    return _fCommandGate->runAction(nativeSetRadioPowerGated, &request);
#else
    if (!fHalService)
        return kIOReturnNotAttached;

    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!ic)
        return kIOReturnNotAttached;

    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (ic->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(kPowerStateOff);
            if (isRunning) {
                net80211_ifstats(ic);
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(kPowerStateOn);
            if (!isRunning) {
                IOReturn ret = enableAdapter(bsdInterface);
                if (ret != kIOReturnSuccess)
                    return ret;
            }
        }
        power_state = (pd->power_state[0]);
    }
    
    return kIOReturnSuccess;
#endif
}

SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
    if (!interface || interface != fNetIf)
        return kIOReturnNotAttached;
    if (!data)
        return kIOReturnBadArgument;
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
}

#if __IO80211_TARGET >= __MAC_15_0
SInt32 AirportItlwm::apple80211_ioctl_get(IO80211SkywalkInterface *interface, void *data, bool, bool)
{
    if (!interface || interface != fNetIf)
        return kIOReturnNotAttached;
    if (!data)
        return kIOReturnBadArgument;

    apple80211req *req = static_cast<apple80211req *>(data);
    return apple80211SkywalkRequest(SIOCGA80211, req->req_type, interface, req->req_data);
}

SInt32 AirportItlwm::apple80211_ioctl_set(IO80211SkywalkInterface *interface, void *data, bool, bool)
{
    if (!interface || interface != fNetIf)
        return kIOReturnNotAttached;
    if (!data)
        return kIOReturnBadArgument;

    apple80211req *req = static_cast<apple80211req *>(data);
    return apple80211SkywalkRequest(SIOCSA80211, req->req_type, interface, req->req_data);
}

SInt32 AirportItlwm::apple80211_ioctl_get(IO80211VirtualInterface *interface, void *data, bool, bool)
{
    if (!data)
        return kIOReturnBadArgument;

    apple80211req *req = static_cast<apple80211req *>(data);
    return apple80211VirtualRequest(SIOCGA80211, req->req_type, interface, req->req_data);
}

SInt32 AirportItlwm::apple80211_ioctl_set(IO80211VirtualInterface *interface, void *data, bool, bool)
{
    if (!data)
        return kIOReturnBadArgument;

    apple80211req *req = static_cast<apple80211req *>(data);
    return apple80211VirtualRequest(SIOCSA80211, req->req_type, interface, req->req_data);
}

bool AirportItlwm::useAppleRSNSupplicant(IO80211VirtualInterface *)
{
#ifdef USE_APPLE_SUPPLICANT
    return true;
#else
    return false;
#endif
}

SInt32 AirportItlwm::apple80211VirtualRequest(UInt request, int cmd, IO80211VirtualInterface *, void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

SInt32 AirportItlwm::setVirtualHardwareAddress(IO80211VirtualInterface *, ether_addr *)
{
    return kIOReturnUnsupported;
}
#endif

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    if (request != SIOCGA80211 && request != SIOCSA80211)
        return kIOReturnError;
    if (!interface || interface != fNetIf)
        return kIOReturnNotAttached;

    AirportItlwmSkywalkInterface *station = OSDynamicCast(AirportItlwmSkywalkInterface, interface);
    if (!station)
        return kIOReturnNotAttached;


    IOReturn ret = kIOReturnUnsupported;
    switch (cmd) {
        case APPLE80211_IOC_SSID:
            if (request == SIOCGA80211)
                ret = station->getSSID(static_cast<apple80211_ssid_data *>(data));
            else
                ret = station->setSSID(static_cast<apple80211_ssid_data *>(data));
            break;
        case APPLE80211_IOC_AUTH_TYPE:
            if (request == SIOCGA80211)
                ret = station->getAUTH_TYPE(static_cast<apple80211_authtype_data *>(data));
            else
                ret = station->setAUTH_TYPE(static_cast<apple80211_authtype_data *>(data));
            break;
        case APPLE80211_IOC_CIPHER_KEY:
            if (request == SIOCSA80211)
                ret = station->setCIPHER_KEY(static_cast<apple80211_key *>(data));
            break;
        case APPLE80211_IOC_CHANNEL:
            if (request == SIOCGA80211)
                ret = station->getCHANNEL(static_cast<apple80211_channel_data *>(data));
            break;
        case APPLE80211_IOC_TXPOWER:
            if (request == SIOCGA80211)
                ret = station->getTXPOWER(static_cast<apple80211_txpower_data *>(data));
            break;
        case APPLE80211_IOC_RATE:
            if (request == SIOCGA80211)
                ret = station->getRATE(static_cast<apple80211_rate_data *>(data));
            break;
        case APPLE80211_IOC_BSSID:
            if (request == SIOCGA80211)
                ret = station->getBSSID(static_cast<apple80211_bssid_data *>(data));
            break;
        case APPLE80211_IOC_SCAN_REQ:
            if (request == SIOCSA80211)
                ret = station->setSCAN_REQ(static_cast<apple80211_scan_data *>(data));
            break;
        case APPLE80211_IOC_SCAN_REQ_MULTIPLE:
            if (request == SIOCSA80211)
                ret = station->setSCAN_REQ_MULTIPLE(static_cast<apple80211_scan_multiple_data *>(data));
            break;
        case APPLE80211_IOC_SCAN_RESULT:
            if (request == SIOCGA80211)
                ret = station->getSCAN_RESULT(static_cast<apple80211_scan_result *>(data));
            break;
        case APPLE80211_IOC_CARD_CAPABILITIES:
            if (request == SIOCGA80211)
                ret = getCARD_CAPABILITIES(interface, static_cast<apple80211_capability_data *>(data));
            break;
        case APPLE80211_IOC_STATE:
            if (request == SIOCGA80211)
                ret = station->getSTATE(static_cast<apple80211_state_data *>(data));
            break;
        case APPLE80211_IOC_PHY_MODE:
            if (request == SIOCGA80211)
                ret = station->getPHY_MODE(static_cast<apple80211_phymode_data *>(data));
            break;
        case APPLE80211_IOC_OP_MODE:
            if (request == SIOCGA80211)
                ret = station->getOP_MODE(static_cast<apple80211_opmode_data *>(data));
            break;
        case APPLE80211_IOC_RSSI:
            if (request == SIOCGA80211)
                ret = station->getRSSI(static_cast<apple80211_rssi_data *>(data));
            break;
        case APPLE80211_IOC_NOISE:
            if (request == SIOCGA80211)
                ret = station->getNOISE(static_cast<apple80211_noise_data *>(data));
            break;
        case APPLE80211_IOC_POWER:
            if (request == SIOCGA80211)
                ret = getPOWER(interface, static_cast<apple80211_power_data *>(data));
            else
                ret = setPOWER(interface, static_cast<apple80211_power_data *>(data));
            break;
        case APPLE80211_IOC_ASSOCIATE:
            if (request == SIOCSA80211)
                ret = station->setASSOCIATE(static_cast<apple80211_assoc_data *>(data));
            break;
        case APPLE80211_IOC_DISASSOCIATE:
            if (request == SIOCSA80211)
                ret = station->setDISASSOCIATE(static_cast<apple80211_disassoc_data *>(data));
            break;
        case APPLE80211_IOC_RATE_SET:
            if (request == SIOCGA80211)
                ret = station->getRATE_SET(static_cast<apple80211_rate_set_data *>(data));
            break;
        case APPLE80211_IOC_MCS_INDEX_SET:
            if (request == SIOCGA80211)
                ret = station->getMCS_INDEX_SET(static_cast<apple80211_mcs_index_set_data *>(data));
            break;
        case APPLE80211_IOC_SUPPORTED_CHANNELS:
        case APPLE80211_IOC_HW_SUPPORTED_CHANNELS:
            if (request == SIOCGA80211)
                ret = station->getSUPPORTED_CHANNELS(static_cast<apple80211_sup_channel_data *>(data));
            break;
        case APPLE80211_IOC_LOCALE:
            if (request == SIOCGA80211)
                ret = station->getLOCALE(static_cast<apple80211_locale_data *>(data));
            break;
        case APPLE80211_IOC_DEAUTH:
            if (request == SIOCGA80211)
                ret = station->getDEAUTH(static_cast<apple80211_deauth_data *>(data));
            else
                ret = station->setDEAUTH(static_cast<apple80211_deauth_data *>(data));
            break;
        case APPLE80211_IOC_DRIVER_VERSION:
            if (request == SIOCGA80211)
                ret = getDRIVER_VERSION(interface, static_cast<apple80211_version_data *>(data));
            break;
        case APPLE80211_IOC_HARDWARE_VERSION:
            if (request == SIOCGA80211)
                ret = getHARDWARE_VERSION(interface, static_cast<apple80211_version_data *>(data));
            break;
        case APPLE80211_IOC_RSN_IE:
            if (request == SIOCGA80211)
                ret = station->getRSN_IE(static_cast<apple80211_rsn_ie_data *>(data));
            else
                ret = station->setRSN_IE(static_cast<apple80211_rsn_ie_data *>(data));
            break;
        case APPLE80211_IOC_AP_IE_LIST:
            if (request == SIOCGA80211)
                ret = station->getAP_IE_LIST(static_cast<apple80211_ap_ie_data *>(data));
            break;
        case APPLE80211_IOC_ASSOCIATION_STATUS:
            if (request == SIOCGA80211)
                ret = station->getASSOCIATION_STATUS(static_cast<apple80211_assoc_status_data *>(data));
            break;
        case APPLE80211_IOC_COUNTRY_CODE:
            if (request == SIOCGA80211)
                ret = getCOUNTRY_CODE(interface, static_cast<apple80211_country_code_data *>(data));
            else
                ret = setCOUNTRY_CODE(interface, static_cast<apple80211_country_code_data *>(data));
            break;
        case APPLE80211_IOC_MCS:
            if (request == SIOCGA80211)
                ret = station->getMCS(static_cast<apple80211_mcs_data *>(data));
            break;
        case APPLE80211_IOC_LINK_CHANGED_EVENT_DATA:
            if (request == SIOCGA80211)
                ret = station->getLINK_CHANGED_EVENT_DATA(static_cast<apple80211_link_changed_event_data *>(data));
            break;
        case APPLE80211_IOC_POWERSAVE:
            if (request == SIOCGA80211)
                ret = station->getPOWERSAVE(static_cast<apple80211_powersave_data *>(data));
            break;
        case APPLE80211_IOC_SCANCACHE_CLEAR:
            if (request == SIOCSA80211)
                ret = station->setSCANCACHE_CLEAR(data);
            break;
        case APPLE80211_IOC_CURRENT_NETWORK:
            if (request == SIOCGA80211)
                ret = station->getCURRENT_NETWORK(static_cast<apple80211_scan_result *>(data));
            break;
        case APPLE80211_IOC_VHT_MCS_INDEX_SET:
            if (request == SIOCGA80211)
                ret = station->getVHT_MCS_INDEX_SET(static_cast<apple80211_vht_mcs_index_set_data *>(data));
            break;
        case APPLE80211_IOC_MCS_VHT:
            if (request == SIOCGA80211)
                ret = station->getMCS_VHT(static_cast<apple80211_mcs_vht_data *>(data));
            break;
        case APPLE80211_IOC_NSS:
            if (request == SIOCGA80211)
                ret = station->getNSS(static_cast<apple80211_nss_data *>(data));
            break;
        case APPLE80211_IOC_WOW_PARAMETERS:
        case APPLE80211_IOC_WOW_ENABLED:
        case APPLE80211_IOC_AWDL_RSDB_CAPS:
            ret = kIOReturnUnsupported;
            break;
        default:
            if (!ml_at_interrupt_context()) {
                const char *name = (cmd >= 0 && static_cast<size_t>(cmd) < ARRAY_SIZE(IOCTL_NAMES)) ? IOCTL_NAMES[cmd] : IOCTL_NAMES[0];
                XYLog("%s Unhandled IOCTL %s (%d) %s\n", __FUNCTION__,
                      name, cmd, request == SIOCGA80211 ? "get" : "set");
            }
            break;
    }
    return ret;
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return apple80211SkywalkRequest(request, cmd, interface, data);
}

IOReturn AirportItlwm::nativeHooksGated(OSObject *owner, void *install, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || !that->fHalService)
        return kIOReturnNotAttached;
    struct ieee80211com *ic = that->fHalService->get80211Controller();
    AirportItlwmSkywalkInterface *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (!ic || !interface)
        return kIOReturnNotAttached;
    struct _ifnet *ifp = &ic->ic_ac.ac_if;
    if (install) {
#if __IO80211_TARGET >= __MAC_15_0
        if (that->fStopping || !interface->nativeTransportAvailable())
            return kIOReturnNotReady;
        if (ifp->native_input && ifp->native_input_context != interface)
            return kIOReturnBusy;
        ifp->native_input_context = interface;
        ifp->native_input = AirportItlwmSkywalkInterface::inputNativePacket;
        ifp->native_output_ready = AirportItlwmSkywalkInterface::outputNativeReady;
        ic->ic_native_controlled = true;
        interface->setNativeControlObserverReady(true);
        that->fNativeHooksInstalled = true;
        return kIOReturnSuccess;
#else
        return kIOReturnUnsupported;
#endif
    }
    if (ifp->native_input_context == interface) {
        ic->ic_native_controlled = false;
        ic->ic_native_join_generation = 0;
#if __IO80211_TARGET >= __MAC_15_0
        ieee80211_clear_native_reassoc(ic);
        interface->setNativeControlObserverReady(false);
#endif
        ifp->native_input = NULL;
        ifp->native_output_ready = NULL;
        ifp->native_input_context = NULL;
    }
    that->fNativeHooksInstalled = false;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::nativeBootstrapGated(OSObject *owner, void *, void *, void *, void *)
{
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || !that->fHalAttached || !that->fHalService ||
        !that->fHalService->get80211Controller())
        return kIOReturnNotAttached;
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (!_fCommandGate || !that->fCommandGateAdded ||
        !that->fNativeHooksInstalled || !interface ||
        !interface->nativeTransportAvailable() || !interface->nativeControlReady())
        return kIOReturnNotReady;
    if (that->fNativeBootstrapDispatched)
        return kIOReturnSuccess;
    if (that->fInterfacePublished)
        return kIOReturnNotReady;

    // The native boot marker updates core availability before reset completion.
    IOReturn availability = that->publishNativeCoreAvailability(true, 0xe0822803, 0x20);
    if (availability != kIOReturnSuccess)
        return availability;
    // Native reset completion loads WCL and Infra capabilities, MAC and channels.
    // postMessage returns void. This records dispatch, not listener completion.
    uint32_t resetComplete = 1;
    interface->postMessage(APPLE80211_M_RESET_INTERFACE, &resetComplete,
                           sizeof(resetComplete), true);
    that->fNativeBootstrapDispatched = true;
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

// Native v3 driver-availability event. Reserved bytes stay zero on normal
// boot and core power transitions, as in the pinned native producer.
struct AiritlNativeDriverAvailableEvent {
    uint32_t version;
    uint32_t flags;
    uint32_t available;
    uint32_t reserved0;
    uint32_t reason;
    uint32_t subReason;
    uint32_t minorReason;
    uint8_t reserved[220];
};
static_assert(sizeof(AiritlNativeDriverAvailableEvent) == 0xf8, "native availability size");
static_assert(__builtin_offsetof(AiritlNativeDriverAvailableEvent, available) == 8, "native availability offset");
static_assert(__builtin_offsetof(AiritlNativeDriverAvailableEvent, reason) == 0x10, "native reason offset");

IOReturn AirportItlwm::publishNativeCoreAvailability(bool available, uint32_t reason, uint32_t flags)
{
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if ((fStopping && available) || !fHalAttached || !fHalService ||
        !fHalService->get80211Controller())
        return kIOReturnNotAttached;
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, fNetIf);
    if (!_fCommandGate || !fCommandGateAdded || !fNativeHooksInstalled || !interface ||
        !interface->nativeTransportAvailable() || (available && !interface->nativeControlReady()))
        return kIOReturnNotReady;
    if (!available && !fNativeAvailabilityDispatched)
        return kIOReturnNotReady;
    if (fNativeAvailabilityDispatched && fNativeCoreAvailable == available &&
        fNativeCoreReason == reason && fNativeCoreFlags == flags)
        return kIOReturnSuccess;

    AiritlNativeDriverAvailableEvent event = {};
    event.version = 3;
    event.flags = flags;
    event.available = available ? 1 : 0;
    event.reason = reason;
    // The inherited ABI returns void. State records dispatch, not delivery.
    interface->postMessage(APPLE80211_M_DRIVER_AVAILABLE, &event, sizeof(event), true);
    fNativeAvailabilityDispatched = true;
    fNativeCoreAvailable = available;
    fNativeCoreReason = reason;
    fNativeCoreFlags = flags;
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwm::nativeCoreEventGated(OSObject *owner, void *controller, void *data, void *, void *)
{
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || !that->fHalService)
        return kIOReturnNotAttached;
    if (!that->fNativeBootstrapDispatched)
        return kIOReturnNotReady;
    auto *ic = static_cast<ieee80211com *>(controller);
    auto *event = static_cast<ieee80211_native_core_event *>(data);
    if (!ic || ic != that->fHalService->get80211Controller() || !event ||
        !event->generation || event->generation != ic->ic_native_core_generation)
        return kIOReturnNotReady;
    if (event->state == IEEE80211_NATIVE_CORE_READY && event->status == 0) {
        if (that->fNativeResetActive) return kIOReturnNotReady;
        if (!(ic->ic_ac.ac_if.if_flags & IFF_RUNNING))
            return kIOReturnNotReady;
        return that->publishNativeCoreAvailability(true, 0xe0821803, 0);
    }
    if ((event->state == IEEE80211_NATIVE_CORE_OFF && event->status == 0) ||
        (event->state == IEEE80211_NATIVE_CORE_FAILED && event->status != 0)) {
        if (ic->ic_ac.ac_if.if_flags & IFF_RUNNING)
            return kIOReturnNotReady;
        return that->publishNativeCoreAvailability(false, 0xe0821804, 0);
    }
    return kIOReturnBadArgument;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwm::nativeResetEventGated(OSObject *owner, void *controller, void *data, void *, void *)
{
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || !that->fHalService || !that->fNativeBootstrapDispatched)
        return kIOReturnNotReady;
    auto *ic = static_cast<ieee80211com *>(controller);
    if (!data) return kIOReturnBadArgument;
    const auto event = *static_cast<const ieee80211_native_reset_event *>(data);
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (!ic || ic != that->fHalService->get80211Controller() ||
        !interface || !interface->nativeTransportAvailable() || !event.identity ||
        !event.origin_core_generation || event.generation != ic->ic_native_core_generation)
        return kIOReturnNotReady;
    if (event.state == IEEE80211_NATIVE_RESET_BEGIN) {
        if (event.status != kIOReturnSuccess || event.identity <= that->fNativeReset.identity ||
            event.generation != event.origin_core_generation ||
            event.hardware_generation != event.origin_hardware_generation ||
            !(ic->ic_if.if_flags & IFF_RUNNING)) return kIOReturnNotReady;
        that->fNativeReset = event;
        that->fNativeResetActive = true;
        interface->nativeResetBegin();
        if (!that->fNativeResetActive || that->fNativeReset.identity != event.identity)
            return kIOReturnAborted;
        ic->ic_native_join_generation = 0;
        ieee80211_clear_native_reassoc(ic);
        ic->ic_des_esslen = 0;
        bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
        bzero(ic->ic_des_bssid, sizeof(ic->ic_des_bssid));
        ic->ic_flags &= ~(IEEE80211_F_DESBSSID | IEEE80211_F_AUTO_JOIN);
        // Do not post APPLE80211_M_RESET_INTERFACE(0) for this internal HAL
        // recovery. In WCL it raises DRIVER_RESET, which halts the scan and join
        // managers; only SYSTEM_POWER_ON or RESUME leave HALTED, and reset
        // completion raises neither (IO80211Family WCLScanManager FSM). The
        // core-off/core-ready availability events and the link-down report
        // already tell WCL the device went away. Completion still posts
        // RESET_INTERFACE(1) to reload capabilities, as the bootstrap does.
        AIRITL_DBG("%s: native reset %llu begin (no WCL driver reset)\n", __FUNCTION__,
              (unsigned long long)event.identity);
        return kIOReturnSuccess;
    }
    const bool cancelledDrain = !that->fNativeResetActive &&
        that->fNativeReset.state == IEEE80211_NATIVE_RESET_CANCELLED &&
        event.state == IEEE80211_NATIVE_RESET_QUIESCED;
    if ((!that->fNativeResetActive && !cancelledDrain) || event.identity != that->fNativeReset.identity ||
        event.origin_core_generation != that->fNativeReset.origin_core_generation ||
        event.origin_hardware_generation != that->fNativeReset.origin_hardware_generation)
        return kIOReturnNotReady;
    if (event.state == IEEE80211_NATIVE_RESET_QUIESCED) {
        if (event.status != (cancelledDrain ? kIOReturnAborted : kIOReturnSuccess) ||
            (!cancelledDrain && that->fNativeReset.state != IEEE80211_NATIVE_RESET_BEGIN) ||
            (ic->ic_if.if_flags & IFF_RUNNING) ||
            event.generation <= event.origin_core_generation ||
            event.hardware_generation <= event.origin_hardware_generation)
            return kIOReturnNotReady;
        that->fNativeReset = event;
        interface->nativeHalQuiesced();
        return kIOReturnSuccess;
    }
    if (event.state == IEEE80211_NATIVE_RESET_RELOADING) {
        if (event.status != kIOReturnSuccess ||
            that->fNativeReset.state != IEEE80211_NATIVE_RESET_QUIESCED ||
            !event.enable_attempt || event.reload_generation != event.generation + 1 ||
            event.generation != that->fNativeReset.generation ||
            event.hardware_generation != that->fNativeReset.hardware_generation)
            return kIOReturnNotReady;
        that->fNativeReset = event;
        return kIOReturnSuccess;
    }
    if (event.state == IEEE80211_NATIVE_RESET_CANCELLED) {
        that->fNativeResetActive = false;
        that->fNativeReset = event;
        interface->nativeResetFinished(false);
        that->publishNativeCoreAvailability(false, 0xe0821804, 0);
        XYLog("%s: native reset %llu cancelled status 0x%x\n", __FUNCTION__,
              (unsigned long long)event.identity, event.status);
        return kIOReturnSuccess;
    }
    if (event.state != IEEE80211_NATIVE_RESET_COMPLETE || event.status != kIOReturnSuccess ||
        that->fNativeReset.state != IEEE80211_NATIVE_RESET_RELOADING ||
        event.enable_attempt != that->fNativeReset.enable_attempt ||
        event.reload_generation != that->fNativeReset.reload_generation ||
        event.generation != event.reload_generation ||
        event.hardware_generation != that->fNativeReset.hardware_generation + 1 ||
        !(ic->ic_if.if_flags & IFF_UP) || !(ic->ic_if.if_flags & IFF_RUNNING) ||
        !that->fHalService->nativeRegulatoryReady()) return kIOReturnNotReady;
    auto *hal = that->fHalService;
    that->fNativeReset = event;
    that->fNativeResetActive = false;
    interface->nativeResetFinished(true);
    auto resetStillCurrent = [&]() {
        return !that->fNativeResetActive && that->fNativeReset.identity == event.identity &&
            !that->fStopping && that->fHalService == hal && that->fNetIf == interface &&
            hal->get80211Controller() == ic &&
            ic->ic_native_core_generation == event.generation &&
            (ic->ic_if.if_flags & IFF_UP) && (ic->ic_if.if_flags & IFF_RUNNING) &&
            interface->nativeControlReady() && hal->nativeRegulatoryReady();
    };
    // WCL's system state manager ignores SSM_EVENT_FW_LOADED while the driver is
    // unavailable (handleIgnore in DEFER_SENDING_NOTIFICATION). Reset completion
    // raises FW_LOADED, and only that event resumes the scan and join managers
    // halted by the reset begin. Publish availability first, as the bootstrap
    // does, so FW_LOADED is queued behind DRIVER_AVAILABLE on WCL's FIFO.
    const IOReturn availability = that->publishNativeCoreAvailability(true, 0xe0821803, 0);
    if (!resetStillCurrent())
        return kIOReturnAborted;
    uint32_t resetComplete = 1;
    interface->postMessage(APPLE80211_M_RESET_INTERFACE, &resetComplete, sizeof(resetComplete), true);
    AIRITL_DBG("%s: native reset %llu complete, availability -> 0x%x\n", __FUNCTION__,
          (unsigned long long)event.identity, availability);
    if (!resetStillCurrent())
        return kIOReturnAborted;
    return availability;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwm::nativeEventGated(OSObject *owner, void *controller, void *message, void *data, void *)
{
#if __IO80211_TARGET >= __MAC_15_0
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || !that->fHalService)
        return kIOReturnNotAttached;
    auto *ic = static_cast<ieee80211com *>(controller);
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (!ic || ic != that->fHalService->get80211Controller() || !interface)
        return kIOReturnNotAttached;
    const int code = int(reinterpret_cast<uintptr_t>(message));
    if (code == IEEE80211_EVT_NATIVE_RESET_STATE)
        return nativeResetEventGated(owner, ic, data, NULL, NULL);
    if (code == IEEE80211_EVT_NATIVE_CORE_STATE)
        return nativeCoreEventGated(owner, ic, data, NULL, NULL);
    if (code == IEEE80211_EVT_NATIVE_BEACON) {
        auto *event = static_cast<ieee80211_native_beacon_event *>(data);
        if (!event || !event->frame || !event->rxinfo)
            return kIOReturnBadArgument;
        const size_t length = (mbuf_flags(event->frame) & MBUF_PKTHDR) ?
            mbuf_pkthdr_len(event->frame) : mbuf_len(event->frame);
        if (length < 36 || length > 36 + 2048)
            return kIOReturnBadArgument;
        uint8_t *bytes = static_cast<uint8_t *>(IOMalloc(length));
        if (!bytes)
            return kIOReturnNoMemory;
        IOReturn result = kIOReturnBadArgument;
        if (mbuf_copydata(event->frame, 0, length, bytes) == 0) {
            AiritlWcl::BeaconInput input = {};
            input.channel = event->channel;
            if (event->channel_flags & IEEE80211_CHAN_2GHZ) input.channelFlags |= APPLE80211_C_FLAG_2GHZ;
            if (event->channel_flags & IEEE80211_CHAN_5GHZ) input.channelFlags |= APPLE80211_C_FLAG_5GHZ;
            input.rssi = event->rxinfo->rxi_rssi + IWX_MIN_DBM;
            input.noiseValid = false;
            if (interface->publishNativeBeacon(bytes, length, input))
                result = kIOReturnSuccess;
        }
        IOFree(bytes, length);
        return result;
    }
    if (code == IEEE80211_EVT_STA_ASSOC_DONE || code == IEEE80211_EVT_STA_DEAUTH ||
        code == IEEE80211_EVT_STA_DISASSOC ||
        code == IEEE80211_EVT_NATIVE_JOIN_FAILURE || code == IEEE80211_EVT_NATIVE_PORT_AUTHORIZED) {
        auto *event = static_cast<ieee80211_native_join_event *>(data);
        if (!event || !event->generation || event->generation != ic->ic_native_join_generation ||
            !ic->ic_bss || memcmp(event->bssid, ic->ic_bss->ni_bssid, IEEE80211_ADDR_LEN))
            return kIOReturnNotReady;
        int kind = code == IEEE80211_EVT_STA_ASSOC_DONE ? 1 :
            code == IEEE80211_EVT_NATIVE_PORT_AUTHORIZED ? 2 :
            code == IEEE80211_EVT_STA_DEAUTH ? 3 :
            code == IEEE80211_EVT_STA_DISASSOC ? 5 : 4;
        interface->handleHalJoinEvent(kind, event->status, event->generation);
        if (kind == 1 || kind == 2)
            that->updateLQMIfChanged();
        else
            that->resetLQM(true);
        return kIOReturnSuccess;
    }
    if (code == IEEE80211_EVT_SCAN_DONE) {
        // Native scans use the HAL callback's captured submission token. This
        // legacy event must never be assigned the newest WCL generation.
        interface->completeScanRequest();
        return kIOReturnSuccess;
    }
    if (code == IEEE80211_EVT_COUNTRY_CODE_UPDATE)
        return interface->publishNativeRegulatoryUpdate();
    return kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

bool AirportItlwm::nativeSystemPowerOn()
{
    // PM admission reads this state outside the main gate too.
    if (fPowerCallLock) IOLockLock(fPowerCallLock);
    const bool on = pmPowerState == kPowerStateOn;
    if (fPowerCallLock) IOLockUnlock(fPowerCallLock);
    return on;
}

IOReturn AirportItlwm::nativeSetRadioPowerGated(OSObject *owner, void *data, void *, void *, void *)
{
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *that = OSDynamicCast(AirportItlwm, owner);
    const auto *input = static_cast<const apple80211_power_data *>(data);
    if (!that || that->fStopping || !that->fHalService || !input)
        return kIOReturnNotAttached;
    if (!input->num_radios) return kIOReturnSuccess;
    auto *iwx = OSDynamicCast(ItlIwx, that->fHalService);
    const bool selected = iwx && iwx->get80211Controller()->ic_native_controlled;
    const uint8_t choice = input->power_state[0];
    auto *hal = that->fHalService;
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    auto *provider = that->getProvider();
    auto *gate = _fCommandGate;
    auto *loop = _fWorkloop;
    struct PowerReferences {
        OSObject *objects[6] = {};
        ~PowerReferences() {
            // Keep the controller alive through every dependent release.
            const unsigned order[] = {2, 1, 3, 4, 5, 0};
            for (unsigned i = 0; i < 6; ++i)
                if (objects[order[i]]) objects[order[i]]->release();
        }
    } references;
    if (selected) {
        IOReturn entry = iwx->nativeRadioInputStatus(gate, loop);
        if (entry != kIOReturnSuccess) return entry;
        if (!provider || !interface || interface->instance != that ||
            interface->fHalService != hal || interface->getProvider() != that ||
            provider != that->fWrapperProvider || provider->getWorkLoop() != loop ||
            !that->fCommandGateAdded) return kIOReturnNotReady;
        if (choice) {
            bool accepted = false;
            entry = iwx->inspectNativeActivation(gate, loop, &accepted);
            if (entry != kIOReturnSuccess) return entry;
        }
        OSObject *objects[] = {that, hal, interface, provider, gate, loop};
        for (unsigned i = 0; i < 6; ++i) {
            references.objects[i] = objects[i]; objects[i]->retain();
        }
    }
    // Publish user intent before any drain releases the gate. A system sleep
    // keeps this choice, but has its own gate-ordered PM state and generation.
    that->power_state = choice;
    if (!++that->fNativePowerSequence) ++that->fNativePowerSequence;
    const uint64_t operation = that->fNativePowerSequence;
    auto current = [&]() {
        return that->fNativePowerSequence == operation && !that->fStopping &&
            !that->controllerTimersClosing() && that->fHalService == hal &&
            that->fNetIf == interface && that->getProvider() == provider &&
            that->fWrapperProvider == provider && provider->getWorkLoop() == loop &&
            _fCommandGate == gate && _fWorkloop == loop && that->fCommandGateAdded &&
            gate->getWorkLoop() == loop && loop->inGate() &&
            interface->getProvider() == that && !hal->isTearingDown();
    };
    that->changePowerStateToPriv(choice ? kPowerStateOn : kPowerStateOff);
    if (selected && !current()) return kIOReturnAborted;
    if (!choice) {
        if (selected) {
            // Keep the legacy wrapper's LQM/watchdog effects, but return the
            // selected source quiesce result to the actual error frontdoor.
            that->resetLQM(true);
            if (!current()) return kIOReturnAborted;
            if (that->watchdogTimer) {
                that->watchdogTimer->cancelTimeout();
                that->watchdogTimer->disable();
            }
            return nativeQuiesceGated(owner, NULL, NULL, NULL, NULL);
        }
        that->disableAdapter(that->bsdInterface);
        return kIOReturnSuccess;
    }
    return that->nativeSystemPowerOn() ?
        nativeEnableGated(owner, that->bsdInterface, NULL, NULL, NULL) : kIOReturnSuccess;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwm::nativeAbortJoinGated(OSObject *owner, void *, void *, void *, void *)
{
#if __IO80211_TARGET >= __MAC_15_0
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || !that->fHalService)
        return kIOReturnNotAttached;
    auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    if (!interface)
        return kIOReturnNotAttached;
    auto *hal = that->fHalService;
    auto *ic = hal->get80211Controller();
#if !defined(AIRITL_ENABLE_BSDIF)
    auto *iwx = OSDynamicCast(ItlIwx, hal);
    if (iwx && ic && ic->ic_native_controlled) {
        IOReturn entry = iwx->nativeExternalControlStatus(_fCommandGate, _fWorkloop);
        if (entry != kIOReturnSuccess) return entry;
    }
#endif
    const uint64_t joinGeneration = interface->nativeJoinGeneration();
    if (!++that->fNativePowerSequence) ++that->fNativePowerSequence;
    const uint64_t operation = that->fNativePowerSequence;
    const bool resumeRadio = that->power_state != 0 && that->nativeSystemPowerOn();
    that->retain(); interface->retain(); hal->retain();
    auto finish = [&](IOReturn status) {
        hal->release(); interface->release(); that->release(); return status;
    };
    auto ownsDrain = [&]() {
        return !that->fStopping && that->fNativePowerSequence == operation &&
            that->fHalService == hal && that->fNetIf == interface &&
            hal->get80211Controller() == ic;
    };
    auto clearJoinTarget = [&]() {
        if (!ic) return;
        ic->ic_native_join_generation = 0;
        ieee80211_clear_native_reassoc(ic);
        ic->ic_des_esslen = 0;
        bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
        bzero(ic->ic_des_bssid, sizeof(ic->ic_des_bssid));
        ic->ic_flags &= ~(IEEE80211_F_DESBSSID | IEEE80211_F_AUTO_JOIN);
    };
    // Apple's leave and join abort (AppleBCMWLANNetAdapter::leaveNetworkSync,
    // AppleBCMWLANJoinAdapter::abortFirmwareJoinSync) disassociate in firmware
    // and keep it running; only a failed disassociation resets the chip. Do
    // the same: tear the station down without a restart, so firmware, LAR/MCC
    // and the scan cache survive and WCL can rejoin at once. Fall back to the
    // full stop/start below if the teardown does not complete cleanly.
    if (resumeRadio) {
        // May release the gate while the newstate task runs.
        IOReturn soft = hal->nativeSoftLeave(2000000000ULL);
        if (soft == kIOReturnSuccess) {
            if (!ownsDrain() || interface->nativeJoinGeneration() != joinGeneration)
                return finish(kIOReturnAborted);
            clearJoinTarget();
            interface->nativeHalQuiesced();
            AIRITL_DBG("%s: soft leave complete (radio kept)\n", __FUNCTION__);
            return finish(kIOReturnSuccess);
        }
        XYLog("%s: soft leave failed 0x%x, full stop\n", __FUNCTION__, soft);
        if (!ownsDrain() || interface->nativeJoinGeneration() != joinGeneration)
            return finish(kIOReturnAborted);
    }
    interface->setNativeControlObserverReady(false);
    // Stop firmware management work before admitting a new join generation.
    // disable may release the gate. A newer power/abort/stop owns its own drain.
    IOReturn result = hal->disable(that->bsdInterface);
    if (result != kIOReturnSuccess) return finish(result);
    if (!ownsDrain() || interface->nativeJoinGeneration() != joinGeneration)
        return finish(kIOReturnAborted);
    clearJoinTarget();
    interface->nativeHalQuiesced();
    if (!ownsDrain()) return finish(kIOReturnAborted);
    if (!resumeRadio || !that->power_state || !that->nativeSystemPowerOn())
        return finish(kIOReturnSuccess);
    return finish(nativeEnableGated(owner, that->bsdInterface, NULL, NULL, NULL));
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportItlwm::abortNativeJoin()
{
    return _fCommandGate && fCommandGateAdded ?
        _fCommandGate->runAction(nativeAbortJoinGated) : kIOReturnNotReady;
}

IOReturn AirportItlwm::nativeQuiesceGated(OSObject *owner, void *finalStop, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that)
        return kIOReturnBadArgument;
    AirportItlwmSkywalkInterface *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
    auto *hal = that->fHalService;
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *iwx = OSDynamicCast(ItlIwx, hal);
    if (iwx && iwx->get80211Controller()->ic_native_controlled) {
        IOReturn entry = iwx->nativeExternalControlStatus(_fCommandGate, _fWorkloop);
        if (entry != kIOReturnSuccess) return entry;
    }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    if (!++that->fNativePowerSequence) ++that->fNativePowerSequence;
    const uint64_t operation = that->fNativePowerSequence;
    that->retain();
    if (interface) interface->retain();
    if (hal) hal->retain();
    if (interface) {
        interface->setNativeControlObserverReady(false);
        interface->cancelNativeRequests(kIOReturnAborted);
    }
#endif
    // A radio power cycle keeps the framework's nexus state. Full teardown
    // closes source admission before stopping the queues' HAL recipient.
#if __IO80211_TARGET >= __MAC_15_0
    IOReturn result = kIOReturnSuccess;
    if (finalStop && interface)
        result = interface->retireDatapath();
    if (that->fNativePowerSequence != operation || that->fHalService != hal ||
        that->fNetIf != interface) result = kIOReturnAborted;
    else if (result == kIOReturnSuccess)
#else
    if (finalStop && interface)
        interface->disableDatapath();
    IOReturn result;
#endif
        result = !hal ? kIOReturnSuccess : finalStop ?
            (hal->quiesceForDetach() ? kIOReturnSuccess : kIOReturnNotReady) :
            hal->disable(that->bsdInterface);
#if __IO80211_TARGET >= __MAC_15_0
    if (that->fNativePowerSequence != operation || that->fHalService != hal ||
        that->fNetIf != interface) result = kIOReturnAborted;
    if (interface && result == kIOReturnSuccess) {
        // Teardown rejects HAL callbacks once fStopping is set. The completed
        // disable still publishes the final down state while this child lives.
        if (that->fNativeAvailabilityDispatched)
            that->publishNativeCoreAvailability(false, 0xe0821804, 0);
        // Publication can reenter another operation. Only this drain may clear
        // its local requests, never a successor's state.
        if (that->fNativePowerSequence == operation && that->fHalService == hal &&
            that->fNetIf == interface) interface->nativeHalQuiesced();
        else result = kIOReturnAborted;
    }
    if (hal) hal->release();
    if (interface) interface->release();
    that->release();
#endif
    return result;
}

IOReturn AirportItlwm::nativeEnableGated(OSObject *owner, void *legacyInterface, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || that->fStopping || that->controllerTimersClosing() ||
        !that->fHalService || !that->watchdogTimer)
        return kIOReturnNotAttached;
    struct RadioReferences {
        OSObject *controller = NULL, *interface = NULL, *hal = NULL;
        OSObject *provider = NULL, *gate = NULL, *loop = NULL;
        ~RadioReferences() {
            if (loop) loop->release();
            if (gate) gate->release();
            if (provider) provider->release();
            if (hal) hal->release();
            if (interface) interface->release();
            if (controller) controller->release();
        }
    } references;
    IONetworkInterface *netif = static_cast<IONetworkInterface *>(legacyInterface);
#if __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    auto *iwx = OSDynamicCast(ItlIwx, that->fHalService);
    if (!netif && iwx && iwx->get80211Controller()->ic_native_controlled) {
        auto *hal = that->fHalService;
        auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
        auto *ic = hal->get80211Controller();
        auto *gate = _fCommandGate;
        auto *loop = _fWorkloop;
        auto *provider = that->getProvider();
        IOReturn entry = iwx->nativeExternalControlStatus(gate, loop);
        if (entry != kIOReturnSuccess) return entry;
        if (!provider || provider != that->fWrapperProvider ||
            that->fWrapperProvider->getWorkLoop() != loop || !that->fCommandGateAdded ||
            !interface || interface->getProvider() != that || interface->instance != that ||
            interface->fHalService != hal || !that->fNativeHooksInstalled || !that->fpNetStats ||
            ic->ic_ac.ac_if.native_input != AirportItlwmSkywalkInterface::inputNativePacket ||
            ic->ic_ac.ac_if.native_input_context != interface ||
            !interface->nativeTransportAvailable()) return kIOReturnNotReady;
        if (!that->power_state || !that->nativeSystemPowerOn() ||
            interface->fNativeResetPending || !hal->supportsNativeScan())
            return kIOReturnNotReady;
        bool accepted = false;
        IOReturn result = iwx->inspectNativeActivation(gate, loop, &accepted);
        if (result != kIOReturnSuccess) return result;
        // An existing producer does not authorize reopening a closed observer.
        if (accepted) {
            if (!interface->nativeControlReady()) return kIOReturnNotReady;
            return that->armControllerWatchdog() ? kIOReturnSuccess : kIOReturnNotReady;
        }
        references.controller = that; references.interface = interface; references.hal = hal;
        references.provider = provider; references.gate = gate; references.loop = loop;
        that->retain(); interface->retain(); hal->retain();
        provider->retain(); gate->retain(); loop->retain();
        if (!++that->fNativePowerSequence) ++that->fNativePowerSequence;
        const uint64_t operation = that->fNativePowerSequence;
        auto current = [&]() {
            return !that->fStopping && !that->controllerTimersClosing() &&
                that->fNativePowerSequence == operation && that->fHalService == hal &&
                that->fNetIf == interface && that->getProvider() == provider &&
                that->fWrapperProvider == provider && provider->getWorkLoop() == loop &&
                _fCommandGate == gate && _fWorkloop == loop && that->fCommandGateAdded &&
                gate->getWorkLoop() == loop && loop->inGate() &&
                interface->getProvider() == that && interface->instance == that &&
                interface->fHalService == hal && hal->get80211Controller() == ic &&
                !hal->isTearingDown() && that->fNativeHooksInstalled &&
                ic->ic_ac.ac_if.native_input == AirportItlwmSkywalkInterface::inputNativePacket &&
                ic->ic_ac.ac_if.native_input_context == interface &&
                interface->nativeTransportAvailable() && !interface->fNativeResetPending &&
                that->power_state && that->nativeSystemPowerOn();
        };
        interface->setNativeControlObserverReady(false);
        result = hal->disable(netif);
        if (!current()) return kIOReturnAborted;
        if (result != kIOReturnSuccess) return result;
        if (!iwx->nativeActivationDrained()) return kIOReturnNotReady;
        interface->nativeHalQuiesced();
        if (!current()) return kIOReturnAborted;
        result = hal->enable(netif);
        if (!current()) return kIOReturnAborted;
        if (result == kIOReturnSuccess) {
            // Main is still held. A queued init cannot run before its source
            // observer and existing control-admission predicates are ready.
            interface->setNativeControlObserverReady(true);
            if (interface->nativeControlReady())
                return that->armControllerWatchdog() ? kIOReturnSuccess : kIOReturnNotReady;
            interface->setNativeControlObserverReady(false);
            result = kIOReturnNotReady;
        }
        // Save the initiating result before stop overwrites the HAL attempt.
        const IOReturn failure = result;
        result = hal->disable(netif);
        if (!current()) return kIOReturnAborted;
        if (result != kIOReturnSuccess) return result;
        interface->nativeHalQuiesced();
        if (!current()) return kIOReturnAborted;
        return failure;
    }
#endif
#ifdef AIRITL_ENABLE_BSDIF
    if (!netif)
        return kIOReturnNotAttached;
#else
    if (!netif) {
#if __IO80211_TARGET >= __MAC_15_0
        AirportItlwmSkywalkInterface *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
        struct ieee80211com *ic = that->fHalService->get80211Controller();
        if (!that->fNativeHooksInstalled || !interface || !ic ||
            ic->ic_ac.ac_if.native_input != AirportItlwmSkywalkInterface::inputNativePacket ||
            ic->ic_ac.ac_if.native_input_context != interface ||
            !interface->nativeTransportAvailable())
            return kIOReturnNotReady;
        if (!that->power_state || !that->nativeSystemPowerOn())
            return kIOReturnNotReady;
        if (!++that->fNativePowerSequence) ++that->fNativePowerSequence;
        const uint64_t operation = that->fNativePowerSequence;
        auto *hal = that->fHalService;
        if (hal->nativeRadioStopPending()) {
            // A newer On request joins an older drain instead of losing its
            // request to Busy. The old continuation cannot resume this radio.
            references.controller = that; references.interface = interface; references.hal = hal;
            that->retain(); interface->retain(); hal->retain();
            IOReturn drained = hal->disable(that->bsdInterface);
            const bool current = drained == kIOReturnSuccess && !that->fStopping &&
                that->fNativePowerSequence == operation && that->fHalService == hal &&
                that->fNetIf == interface && hal->get80211Controller() == ic &&
                that->power_state && that->nativeSystemPowerOn();
            if (current) interface->nativeHalQuiesced();
            const bool owned = current && that->fNativePowerSequence == operation &&
                !that->fStopping && that->fHalService == hal && that->fNetIf == interface &&
                that->power_state && that->nativeSystemPowerOn();
            if (!owned) return drained == kIOReturnSuccess ? kIOReturnAborted : drained;
        }
        interface->setNativeControlObserverReady(true);
        if (!interface->nativeControlReady()) {
            interface->setNativeControlObserverReady(false);
            return kIOReturnNotReady;
        }
#else
        return kIOReturnNotAttached;
#endif
    }
#endif
    if (!that->fHalService->get80211Controller() || !that->fpNetStats)
        return kIOReturnNotAttached;

    IOReturn ret = that->fHalService->enable(netif);
    if (ret != kIOReturnSuccess) {
#if __IO80211_TARGET >= __MAC_15_0
        auto *interface = OSDynamicCast(AirportItlwmSkywalkInterface, that->fNetIf);
        if (interface) interface->setNativeControlObserverReady(false);
#endif
        return ret;
    }

    if (!that->armControllerWatchdog()) return kIOReturnNotReady;
    return kIOReturnSuccess;
}

// The IONetworkController open path. The V2 header leaves the Skywalk overloads
// non-virtual inline stubs, so this is where the stack actually enables the
// interface: run the base plumbing, then the adapter and the watchdog.
IOReturn AirportItlwm::enable(IONetworkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    // Qualified: IO80211Controller shadows the IONetworkController overloads.
    IOReturn ret = IONetworkController::enable(netif);
    if (ret != kIOReturnSuccess)
        return ret;
    if (_fCommandGate)
        _fCommandGate->enable();
    // Enabling the interface owns the radio. Do not wait for setPOWER: that
    // path skips the adapter whenever the interface is already running, and the
    // gated body refuses while power_state is still 0, so a cold boot would
    // otherwise never power the HAL.
    if (!power_state) {
        power_state = 1;
        changePowerStateToPriv(kPowerStateOn);
    }
    {
        // Under native control the HAL runs the switch itself, and the gated
        // body takes that branch only when the legacy client pointer is null.
        auto *iwx = OSDynamicCast(ItlIwx, fHalService);
        const bool native = iwx && iwx->get80211Controller() &&
            iwx->get80211Controller()->ic_native_controlled;
        ret = enableAdapter(native ? NULL : netif);
        if (ret != kIOReturnSuccess)
            return ret;
    }
    if (!armControllerWatchdog())
        IOLog("AirportItlwm::enable watchdog arm failed\n");
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IONetworkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    {
        OSString *ready = OSString::withCString("FALSE");
        if (ready) {
            if (fNetIf) fNetIf->setProperty("CoreWiFiDriverReadyKey", ready);
            setProperty("CoreWiFiDriverReadyKey", ready);
            ready->release();
        }
    }
    if (power_state) {
        // Symmetric to enable: the disable owns the radio going quiet.
        power_state = 0;
        changePowerStateToPriv(kPowerStateOff);
        disableAdapter(netif);
    }
    setLinkStatus(kIONetworkLinkValid);
    return IONetworkController::disable(netif);
}

IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif)
{
    if (!_fCommandGate || !fCommandGateAdded)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(nativeEnableGated, netif);
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    resetLQM(true);

    if (watchdogTimer) {
        watchdogTimer->cancelTimeout();
        watchdogTimer->disable();
    }
    if (_fCommandGate && fCommandGateAdded)
        _fCommandGate->runAction(nativeQuiesceGated, NULL);
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    IOCommandGate *gate = _fCommandGate;
    if (!gate)
        return kIOReturnNotAttached;
    
    if (arg1 == 0) {
        if (gate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (gate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
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
    IOService *maker = NULL;
    if (drained) {
        maker = pmPolicyMaker;
        pmPolicyMaker = NULL;
        fPowerCallOwnerHeld = false;
    }
    IOLockUnlock(fPowerCallLock);
    if (maker) maker->release();
    if (held) release();
    return drained;
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
    if (!fPowerCallLock) return result;
    IOLockLock(fPowerCallLock);
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    if (fPowerCallAdmission && !fPowerCallStopping) {
        if ((powerStateOrdinal != kPowerStateOff && powerStateOrdinal != kPowerStateOn) ||
            !pmPolicyMaker || policyMaker != pmPolicyMaker) {
            IOLockUnlock(fPowerCallLock);
            return kIOReturnBadArgument;
        }
        fLatestRequestedPowerState = static_cast<UInt32>(powerStateOrdinal);
    }
#endif
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
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    const bool execute = admitted && self->fLatestRequestedPowerState == kPowerStateOff &&
        self->pmPowerState != kPowerStateOff;
    if (execute) self->pmPowerState = kPowerStateOff;
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) {
        if (execute) self->setPowerStateOff();
    } else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
#else
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) self->setPowerStateOff();
    else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
#endif
    return kIOReturnSuccess;
}

static IOReturn handleSetPowerStateOnGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = OSDynamicCast(AirportItlwm, owner);
    if (!self) return kIOReturnBadArgument;
    IOLockLock(self->fPowerCallLock);
    const bool admitted = self->fPowerCallAdmission && !self->fPowerCallStopping;
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    const bool execute = admitted && self->fLatestRequestedPowerState == kPowerStateOn &&
        self->pmPowerState != kPowerStateOn;
    if (execute) self->pmPowerState = kPowerStateOn;
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) {
        if (execute) self->setPowerStateOn();
    } else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
#else
    IOLockUnlock(self->fPowerCallLock);
    if (admitted) self->setPowerStateOn();
    else if (self->pmPolicyMaker) self->pmPolicyMaker->acknowledgeSetPowerState();
#endif
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<AirportItlwm *>(param0);
    AirportItlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    // Refusal leaves the request outstanding until the public PM timeout;
    // the log is the only record.
    IOReturn result = gate ? gate->runAction(handleSetPowerStateOffGated) : kIOReturnNotReady;
    if (result != kIOReturnSuccess) {
        if (!gate) IOLog("AirportItlwm::PM Off dispatch refused gate=missing\n");
        else IOLog("AirportItlwm::PM Off dispatch refused status=0x%x\n", result);
    }
    self->endPowerCall(&frame);
    self->release();
}

static void handleSetPowerStateOn(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<AirportItlwm *>(param0);
    AirportItlwm::PowerCallFrame frame;
    self->beginPowerCall(&frame);
    IOCommandGate *gate = self->getCommandGate();
    // Refusal leaves the request outstanding until the public PM timeout;
    // the log is the only record.
    IOReturn result = gate ? gate->runAction(handleSetPowerStateOnGated) : kIOReturnNotReady;
    if (result != kIOReturnSuccess) {
        if (!gate) IOLog("AirportItlwm::PM On dispatch refused gate=missing\n");
        else IOLog("AirportItlwm::PM On dispatch refused status=0x%x\n", result);
    }
    self->endPowerCall(&frame);
    self->release();
}

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    if (!policyMaker) return kIOReturnBadArgument;
    if (!fPowerCallLock) return kIOReturnNoMemory;
    // The registration frame owns the maker through unlocked work and unwind.
    policyMaker->retain();
    IOLockLock(fPowerCallLock);
    const bool busy = fPowerCallStopping || fPowerCallDraining ||
        powerOffThreadCall || powerOnThreadCall || fPowerCallOwnerHeld || pmPolicyMaker;
    if (busy) {
        IOLockUnlock(fPowerCallLock);
        policyMaker->release();
        return kIOReturnNotReady;
    }
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF)
    fLatestRequestedPowerState = kPowerStateOn;
#endif
    pmPowerState = kPowerStateOn;
    // The published field owns a separate reference until native-call drain.
    policyMaker->retain();
    pmPolicyMaker = policyMaker;
    powerOffThreadCall = thread_call_allocate(handleSetPowerStateOff, this);
    powerOnThreadCall = thread_call_allocate(handleSetPowerStateOn, this);
    if (!powerOffThreadCall || !powerOnThreadCall) {
        IOLockUnlock(fPowerCallLock);
        // Returning an error does not claim cleanup finished: an incomplete
        // drain leaves its existing ownership for a later drain. No owner
        // retain exists yet on this branch, only the maker reference.
        const bool drained = unregistPM(false);
        if (!drained) IOLog("AirportItlwm::PM allocation unwind drain did not finish; maker reference kept for later drain\n");
        policyMaker->release();
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
    if (result != kIOReturnSuccess || stopped) {
        // Returning an error does not claim cleanup finished: an incomplete
        // drain keeps the owner retain and maker reference for a later drain.
        const bool drained = unregistPM(false);
        if (!drained) IOLog("AirportItlwm::PM registration unwind drain did not finish; owner retain and maker reference kept for later drain\n");
    }
    if (stopped) result = kIOReturnAborted;
    policyMaker->release();
    release();
    return result;
}

void AirportItlwm::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    if (!++fNativePowerSequence) ++fNativePowerSequence;
#endif
#if !(defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF))
    if (fPowerCallLock)
        IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOff;
    if (fPowerCallLock)
        IOLockUnlock(fPowerCallLock);
#endif
    disableAdapter(bsdInterface);
    if (pmPolicyMaker)
        pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportItlwm::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
#if __IO80211_TARGET >= __MAC_15_0
    if (!++fNativePowerSequence) ++fNativePowerSequence;
#endif
#if !(defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF))
    if (fPowerCallLock)
        IOLockLock(fPowerCallLock);
    pmPowerState = kPowerStateOn;
    if (fPowerCallLock)
        IOLockUnlock(fPowerCallLock);
#endif
    // System sleep stops the HAL without changing the user's radio choice.
    // Restore that choice when power returns; an explicitly off radio stays off.
    if (power_state && fHalAttached && !fStopping) {
        IOReturn ret = enableAdapter(bsdInterface);
        if (ret != kIOReturnSuccess)
            XYLog("%s resume radio failed 0x%x\n", __FUNCTION__, ret);
    }
    if (pmPolicyMaker)
        pmPolicyMaker->acknowledgeSetPowerState();
}
