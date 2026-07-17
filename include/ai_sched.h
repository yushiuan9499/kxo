#pragma once

#include <linux/sched/loadavg.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "ai-game.h"

#define LOAD_1US (1 << 10)
#define LOAD_1MS (1 << 20)
extern u64 ai_load_ns[XO_AI_TOT];
extern spinlock_t load_lock;

DECLARE_PER_CPU(unsigned, work_done);
DECLARE_PER_CPU(ktime_t, done_time);

int init_ai_sched(void);
void sched_games(unsigned long unfini, struct ai_game *games);
void free_ai_sched(void);

#define EXP_10_MATCH 1853
#define commit_load(cpu, who, nsecs)                                        \
    do {                                                                    \
        u64 scaled_nsecs =                                                  \
            (nsecs * arch_scale_cpu_capacity(cpu)) >> SCHED_CAPACITY_SHIFT; \
        spin_lock_bh(&load_lock);                                           \
        ai_load_ns[who] =                                                   \
            calc_load(ai_load_ns[who], EXP_10_MATCH, scaled_nsecs);         \
        spin_unlock_bh(&load_lock);                                         \
    } while (0)
