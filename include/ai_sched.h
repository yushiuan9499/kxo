#pragma once

#include <linux/sched/loadavg.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "ai-game.h"

typedef u64 load_t;
#define LOAD_1US FIXED_1
#define LOAD_1MS (LOAD_1US << 10)
#define NS_LOAD_RATIO (FSHIFT - 10)

extern load_t ai_load[XO_AI_TOT];
extern spinlock_t load_lock;

int init_ai_sched(void);
void sched_games(unsigned long unfini, struct ai_game *games);
void free_ai_sched(void);

#if defined(CONFIG_X86_64) || defined(CONFIG_X86_32)
extern unsigned long *calculated_capacity;
#undef arch_scale_cpu_capacity
#define arch_scale_cpu_capacity(cpu) (calculated_capacity[cpu])
#endif

#define commit_load(cpu, who, nsecs)                                        \
    do {                                                                    \
        load_t scaled_nsecs =                                               \
            (nsecs * arch_scale_cpu_capacity(cpu)) >> SCHED_CAPACITY_SHIFT; \
        spin_lock_bh(&load_lock);                                           \
        ai_load[who] =                                                      \
            calc_load(ai_load[who], EXP_1, scaled_nsecs << NS_LOAD_RATIO);  \
        spin_unlock_bh(&load_lock);                                         \
    } while (0)
