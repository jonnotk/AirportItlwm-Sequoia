//
//  _task.cpp
//  itlwm
//
//  Created by qcwap on 2020/3/1.
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

#include <sys/_task.h>
#include <sys/proc.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>

enum ETQ_STATE {
    TQ_S_CREATED,
    TQ_S_RUNNING,
    TQ_S_DESTROYED
};

struct taskq_active {
    TAILQ_ENTRY(taskq_active) entry;
    struct task *source;
    thread_t thread;
};
TAILQ_HEAD(taskq_active_list, taskq_active);

struct taskq {
    enum ETQ_STATE       tq_state;
    unsigned int         tq_running;
    unsigned int         tq_waiting;
    unsigned int         tq_nthreads;
    unsigned int         tq_flags;
    const char        *tq_name;

    IORecursiveLock *tq_mtx;
    struct task_list     tq_worklist;
    struct taskq_active_list tq_active;
    bool tq_creator_active;
};

static const char taskq_sys_name[] = "systq";

struct taskq taskq_sys = {
    TQ_S_CREATED,
    0,
    0,
    1,
    0,
    taskq_sys_name,
};

struct taskq *const systq = &taskq_sys;

int
taskq_next_work(struct taskq *tq, struct task *work, struct taskq_active *active)
{
    struct task *next;
    
    //    IOLog("itlwm: taskq %s lock\n", __FUNCTION__);
    IORecursiveLockLock(tq->tq_mtx);
    
    while ((next = TAILQ_FIRST(&tq->tq_worklist)) == NULL) {
        if (tq->tq_state != TQ_S_RUNNING) {
            IORecursiveLockUnlock(tq->tq_mtx);
            return (0);
        }
        IORecursiveLockSleep(tq->tq_mtx, tq, THREAD_INTERRUPTIBLE);
    }

    TAILQ_REMOVE(&tq->tq_worklist, next, t_entry);
    CLR(next->t_flags, TASK_ONQUEUE);

    *work = *next; /* copy to caller to avoid races */
    active->source = next;
    active->thread = current_thread();
    TAILQ_INSERT_TAIL(&tq->tq_active, active, entry);

    next = TAILQ_FIRST(&tq->tq_worklist);
    IORecursiveLockUnlock(tq->tq_mtx);
//    IOLog("itlwm: taskq %s unlock\n", __FUNCTION__);

    if (next != NULL && tq->tq_nthreads > 1)
        IORecursiveLockWakeup(tq->tq_mtx, tq, true);

    return (1);
}

void
taskq_thread(void *xtq)
{
    struct taskq *tq = (struct taskq *)xtq;
    struct task work;
    struct taskq_active active;
    int last;

//    if (ISSET(tq->tq_flags, TASKQ_MPSAFE))
//        KERNEL_UNLOCK();

//    WITNESS_CHECKORDER(&tq->tq_lock_object, LOP_NEWORDER, NULL);
    
    IOLog("itlwm: taskq %s schedule task\n", __FUNCTION__);

    while (taskq_next_work(tq, &work, &active)) {
//        WITNESS_LOCK(&tq->tq_lock_object, 0);
//        IOLog("itlwm: taskq worker thread=%lld work=%s\n", thread_tid(current_thread()), work.name);
        (*work.t_func)(work.t_arg);
        IORecursiveLockLock(tq->tq_mtx);
        TAILQ_REMOVE(&tq->tq_active, &active, entry);
        IORecursiveLockWakeup(tq->tq_mtx, tq, false);
        IORecursiveLockUnlock(tq->tq_mtx);
//        IOLog("itlwm: taskq worker thread=%lld work=%s done", thread_tid(current_thread()), work.name);
//        WITNESS_UNLOCK(&tq->tq_lock_object, 0);
//        sched_pause(yield);
        IOSleep(1);
    }
    
    IOLog("itlwm: taskq %s schedule task done\n", __FUNCTION__);

    IORecursiveLockLock(tq->tq_mtx);
    last = (--tq->tq_running == 0);
    if (last)
        IORecursiveLockWakeup(tq->tq_mtx, tq, false);
    IORecursiveLockUnlock(tq->tq_mtx);

//    if (ISSET(tq->tq_flags, TASKQ_MPSAFE))
//        KERNEL_LOCK();

    if (last) {
        IOLog("itlwm: taskq %s schedule task wakeup\n", __FUNCTION__);
    }

//    kthread_exit(0);
    thread_terminate(current_thread());
}

void taskq_create_thread(void *arg)
{
    struct taskq *tq = (struct taskq *)arg;
    int rv;
    IOLog("itlwm: taskq %s lock\n", __FUNCTION__);
    IORecursiveLockLock(tq->tq_mtx);
    switch (tq->tq_state) {
        case TQ_S_DESTROYED:
            tq->tq_creator_active = false;
            IORecursiveLockWakeup(tq->tq_mtx, tq, false);
            IOLog("itlwm: taskq %s unlock\n", __FUNCTION__);
            IORecursiveLockUnlock(tq->tq_mtx);
            return;

        case TQ_S_CREATED:
            tq->tq_state = TQ_S_RUNNING;
            break;

        default:
            IOLog("itlwm: unexpected %s tq state %u", tq->tq_name, tq->tq_state);
            tq->tq_creator_active = false;
            IORecursiveLockWakeup(tq->tq_mtx, tq, false);
            IORecursiveLockUnlock(tq->tq_mtx);
            return;
    }

    do {
        tq->tq_running++;
        IOLog("itlwm: taskq %s unlock\n", __FUNCTION__);
        IORecursiveLockUnlock(tq->tq_mtx);

        thread_t new_thread;
        rv = kernel_thread_start((thread_continue_t)taskq_thread, tq, &new_thread);
        if (rv == KERN_SUCCESS)
            thread_deallocate(new_thread);

        IOLog("itlwm: taskq %s lock\n", __FUNCTION__);
        IORecursiveLockLock(tq->tq_mtx);
        if (rv != KERN_SUCCESS) {
            IOLog("itlwm: tasq unable to create thread for \"%s\" taskq\n",
                   tq->tq_name);

            tq->tq_running--;
            /* could have been destroyed during kthread_create */
            if (tq->tq_state == TQ_S_DESTROYED &&
                tq->tq_running == 0)
                IORecursiveLockWakeup(tq->tq_mtx, tq, false);
            break;
        }
    } while (tq->tq_state == TQ_S_RUNNING && tq->tq_running < tq->tq_nthreads);
    
    IOLog("itlwm: taskq %s unlock\n", __FUNCTION__);
    tq->tq_creator_active = false;
    IORecursiveLockWakeup(tq->tq_mtx, tq, false);
    IORecursiveLockUnlock(tq->tq_mtx);
}

static bool
taskq_await_start(struct taskq *tq)
{
    IORecursiveLockLock(tq->tq_mtx);
    while (tq->tq_creator_active)
        IORecursiveLockSleep(tq->tq_mtx, tq, THREAD_UNINT);
    bool ready = tq->tq_state == TQ_S_RUNNING && tq->tq_running != 0;
    IORecursiveLockUnlock(tq->tq_mtx);
    return ready;
}

bool
taskq_init(void)
{
    systq->tq_state = TQ_S_CREATED;
    systq->tq_running = 0;
    systq->tq_creator_active = true;
    systq->tq_mtx = IORecursiveLockAlloc();
    if (!systq->tq_mtx) {
        systq->tq_creator_active = false;
        return false;
    }
    TAILQ_INIT(&systq->tq_worklist);
    TAILQ_INIT(&systq->tq_active);
    thread_t new_thread;
    if (kernel_thread_start((thread_continue_t)taskq_create_thread, systq, &new_thread) == KERN_SUCCESS)
        thread_deallocate(new_thread);
    else {
        systq->tq_creator_active = false;
    }
    if (!taskq_await_start(systq)) {
        IORecursiveLockFree(systq->tq_mtx);
        systq->tq_mtx = NULL;
        return false;
    }
    return true;
}

struct taskq *
taskq_create(const char *name, unsigned int nthreads, int ipl,
             unsigned int flags)
{
    struct taskq *tq;

    tq = (struct taskq *)IOMalloc(sizeof(*tq));
    if (tq == NULL)
        return (NULL);

    tq->tq_state = TQ_S_CREATED;
    tq->tq_running = 0;
    tq->tq_waiting = 0;
    tq->tq_nthreads = nthreads;
    tq->tq_name = name;
    tq->tq_flags = flags;
    tq->tq_creator_active = true;
    tq->tq_mtx = IORecursiveLockAlloc();
    if (!tq->tq_mtx) {
        IOFree(tq, sizeof(*tq));
        return NULL;
    }

    //    mtx_init_flags(&tq->tq_mtx, ipl, name, 0);
    TAILQ_INIT(&tq->tq_worklist);
    TAILQ_INIT(&tq->tq_active);
    thread_t new_thread;
    /* try to create a thread to guarantee that tasks will be serviced */
    if (kernel_thread_start((thread_continue_t)taskq_create_thread, tq, &new_thread) != KERN_SUCCESS) {
        IORecursiveLockFree(tq->tq_mtx);
        IOFree(tq, sizeof(*tq));
        return NULL;
    }
    thread_deallocate(new_thread);
    if (!taskq_await_start(tq)) {
        IORecursiveLockFree(tq->tq_mtx);
        IOFree(tq, sizeof(*tq));
        return NULL;
    }
    return (tq);
}

void
taskq_destroy(struct taskq *tq)
{
    if (!tq || !tq->tq_mtx) {
        return;
    }
    IORecursiveLockLock(tq->tq_mtx);
    switch (tq->tq_state) {
        case TQ_S_CREATED:
        case TQ_S_RUNNING:
            tq->tq_state = TQ_S_DESTROYED;
            break;

        default:
            IOLog("itlwm: unexpected %s tq state %u", tq->tq_name, tq->tq_state);
            tq->tq_state = TQ_S_DESTROYED;
            IORecursiveLockUnlock(tq->tq_mtx);
            return;
    }

    while (tq->tq_creator_active || tq->tq_running > 0) {
        IORecursiveLockWakeup(tq->tq_mtx, tq, false);
        IORecursiveLockSleep(tq->tq_mtx, tq, THREAD_INTERRUPTIBLE);
    }

    IORecursiveLockUnlock(tq->tq_mtx);
    IORecursiveLockFree(tq->tq_mtx);
    tq->tq_mtx = NULL;
    if (tq != systq) {
        IOFree(tq, sizeof(*tq));
    }
    
}

void
task_set(struct task *t, void (*fn)(void *), void *arg, const char *name)
{
    t->t_func = fn;
    t->t_arg = arg;
    t->t_flags = 0;
    strlcpy(t->name, name, sizeof(t->name));
}

static struct taskq_task_status
taskq_inspect_locked(struct taskq *tq, struct task *w)
{
    struct taskq_task_status status = {};
    status.service = tq->tq_state == TQ_S_RUNNING && tq->tq_running != 0 &&
        !tq->tq_creator_active;
    status.suspended = ISSET(w->t_flags, TASK_SUSPENDED);
    status.pending = ISSET(w->t_flags, TASK_ONQUEUE);
    struct taskq_active *active;
    TAILQ_FOREACH(active, &tq->tq_active, entry)
        if (active->source == w) ++status.active;
    return status;
}

struct taskq_task_status
taskq_inspect_task(struct taskq *tq, struct task *w)
{
    struct taskq_task_status status = {};
    if (!tq || !tq->tq_mtx || !w) return status;
    IORecursiveLockLock(tq->tq_mtx);
    status = taskq_inspect_locked(tq, w);
    IORecursiveLockUnlock(tq->tq_mtx);
    return status;
}

enum taskq_admission
taskq_add_checked(struct taskq *tq, struct task *w, struct taskq_task_status *snapshot)
{
    struct taskq_task_status status = {};
    enum taskq_admission result = TASKQ_REFUSED;
    if (!tq || !tq->tq_mtx || !w) {
        if (snapshot) *snapshot = status;
        return result;
    }
    IORecursiveLockLock(tq->tq_mtx);
    status = taskq_inspect_locked(tq, w);
    if (status.service && !status.suspended) {
        if (status.active) result = TASKQ_ACTIVE;
        else if (status.pending) result = TASKQ_PENDING;
        else {
            SET(w->t_flags, TASK_ONQUEUE);
            TAILQ_INSERT_TAIL(&tq->tq_worklist, w, t_entry);
            status.pending = true;
            result = TASKQ_QUEUED;
        }
    }
    IORecursiveLockUnlock(tq->tq_mtx);
    if (snapshot) *snapshot = status;
    if (result == TASKQ_QUEUED) IORecursiveLockWakeup(tq->tq_mtx, tq, true);
    return result;
}

int
task_add(struct taskq *tq, struct task *w)
{
    int rv = 0;
//    IOLog("itlwm: taskq task_add %s thread: %lld\n", w->name, thread_tid(current_thread()));
    
    IORecursiveLockLock(tq->tq_mtx);
    if (ISSET(w->t_flags, TASK_ONQUEUE | TASK_SUSPENDED) ||
        tq->tq_state == TQ_S_DESTROYED) {
//        IOLog("itlwm: taskq task_add %s is already on queue thread: %lld\n", w->name, thread_tid(current_thread()));
        IORecursiveLockUnlock(tq->tq_mtx);
        return (0);
    }
    if (!ISSET(w->t_flags, TASK_ONQUEUE)) {
//        IOLog("itlwm: taskq task_add %s add to queue thread: %lld\n", w->name, thread_tid(current_thread()));
        rv = 1;
        SET(w->t_flags, TASK_ONQUEUE);
        TAILQ_INSERT_TAIL(&tq->tq_worklist, w, t_entry);
    }
    IORecursiveLockUnlock(tq->tq_mtx);

    if (rv)
        IORecursiveLockWakeup(tq->tq_mtx, tq, true);

    return (rv);
}

void
taskq_suspend_task(struct taskq *tq, struct task *w)
{
    if (!tq || !tq->tq_mtx)
        return;
    IORecursiveLockLock(tq->tq_mtx);
    SET(w->t_flags, TASK_SUSPENDED);
    if (ISSET(w->t_flags, TASK_ONQUEUE)) {
        CLR(w->t_flags, TASK_ONQUEUE);
        TAILQ_REMOVE(&tq->tq_worklist, w, t_entry);
    }
    IORecursiveLockUnlock(tq->tq_mtx);
}

void
taskq_resume_task(struct taskq *tq, struct task *w)
{
    if (!tq || !tq->tq_mtx)
        return;
    IORecursiveLockLock(tq->tq_mtx);
    CLR(w->t_flags, TASK_SUSPENDED);
    IORecursiveLockUnlock(tq->tq_mtx);
}

static bool
taskq_has_other_work(struct taskq *tq, struct task *w)
{
    struct taskq_active *active;
    TAILQ_FOREACH(active, &tq->tq_active, entry) {
        if ((!w || active->source == w) && active->thread != current_thread())
            return true;
    }
    return false;
}

bool
taskq_is_current(struct taskq *tq)
{
    if (!tq || !tq->tq_mtx)
        return false;
    bool found = false;
    IORecursiveLockLock(tq->tq_mtx);
    struct taskq_active *active;
    TAILQ_FOREACH(active, &tq->tq_active, entry)
        if (active->thread == current_thread()) {
            found = true;
            break;
        }
    IORecursiveLockUnlock(tq->tq_mtx);
    return found;
}

void
taskq_del_barrier_gated(struct taskq *tq, struct task *w, IOCommandGate *gate)
{
    if (!tq || !tq->tq_mtx)
        return;
    taskq_suspend_task(tq, w);
    IORecursiveLockLock(tq->tq_mtx);
    while (taskq_has_other_work(tq, w)) {
        if (!gate) {
            IORecursiveLockSleep(tq->tq_mtx, tq, THREAD_UNINT);
        } else {
            IORecursiveLockUnlock(tq->tq_mtx);
            // The queue lock and gate cannot be held across each other while
            // sleeping. A deadline closes the check/sleep lost-wakeup window.
            AbsoluteTime deadline;
            clock_interval_to_deadline(10, kMillisecondScale,
                reinterpret_cast<uint64_t *>(&deadline));
            gate->commandSleep(tq, deadline, THREAD_UNINT);
            IORecursiveLockLock(tq->tq_mtx);
        }
    }
    IORecursiveLockUnlock(tq->tq_mtx);
}

void
taskq_del_barrier(struct taskq *tq, struct task *w)
{
    taskq_del_barrier_gated(tq, w, NULL);
}

void
taskq_barrier(struct taskq *tq)
{
    if (!tq || !tq->tq_mtx)
        return;
    IORecursiveLockLock(tq->tq_mtx);
    while (!TAILQ_EMPTY(&tq->tq_worklist) || taskq_has_other_work(tq, NULL))
        IORecursiveLockSleep(tq->tq_mtx, tq, THREAD_UNINT);
    IORecursiveLockUnlock(tq->tq_mtx);
}

int
task_del(struct taskq *tq, struct task *w)
{
    int rv = 0;
//    IOLog("itlwm: taskq task_del %s thread: %lld\n", w->name, thread_tid(current_thread()));
    
    IORecursiveLockLock(tq->tq_mtx);
    if (ISSET(w->t_flags, TASK_ONQUEUE)) {
        rv = 1;
        CLR(w->t_flags, TASK_ONQUEUE);
        TAILQ_REMOVE(&tq->tq_worklist, w, t_entry);
    }
    IORecursiveLockUnlock(tq->tq_mtx);

    return (rv);
}
