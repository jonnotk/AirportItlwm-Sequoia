//
//  _task.h
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

#ifndef _task_h
#define _task_h

#include <sys/queue.h>
#include <sys/_buf.h>

#include <IOKit/IOLocks.h>

struct task {
    TAILQ_ENTRY(task) t_entry;
    void        (*t_func)(void *);
    void        *t_arg;
    unsigned int    t_flags;
    char name[256];
};

#define TASK_ONQUEUE        1
#define TASK_BARRIER        2
#define TASK_SUSPENDED      4

TAILQ_HEAD(task_list, task);

#define TASKQ_MPSAFE        (1 << 0)

#define TASK_INITIALIZER(_f, _a)  {{ NULL, NULL }, (_f), (_a), 0 }

#define task_pending(_t)    ((_t)->t_flags & TASK_ONQUEUE)

extern struct taskq *const systq;

bool taskq_init();
struct taskq    *taskq_create(const char *, unsigned int, int, unsigned int);
void         taskq_destroy(struct taskq *);
void         taskq_barrier(struct taskq *);

void         taskq_del_barrier(struct taskq *, struct task *);
class IOCommandGate;
// Suspend closes admission and removes pending work atomically. A barrier also
// waits for work already dequeued, excluding the caller's own task frame.
void         taskq_suspend_task(struct taskq *, struct task *);
void         taskq_resume_task(struct taskq *, struct task *);
void         taskq_del_barrier_gated(struct taskq *, struct task *, IOCommandGate *);
bool         taskq_is_current(struct taskq *);

void         task_set(struct task *, void (*)(void *), void *, const char *);
// Snapshot and checked admission share the worker handoff mutex. They do not
// change task_add's historical return convention.
struct taskq_task_status {
    bool service, suspended, pending;
    unsigned active;
};
enum taskq_admission { TASKQ_REFUSED, TASKQ_QUEUED, TASKQ_PENDING, TASKQ_ACTIVE };
struct taskq_task_status taskq_inspect_task(struct taskq *, struct task *);
enum taskq_admission taskq_add_checked(struct taskq *, struct task *,
    struct taskq_task_status *);

int         task_add(struct taskq *, struct task *);
int         task_del(struct taskq *, struct task *);

#endif /* _task_h */
