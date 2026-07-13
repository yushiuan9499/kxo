#pragma once

#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include "rl.h"

struct ai_game;
typedef int (*ai_alg)(struct ai_game *game, char player);
#define AI_RESCHED -2

enum ai_game_state {
    GAME_BUSY,
    GAME_READY,
    GAME_RESCHED,
    GAME_DONE,
};

struct ai_avg {
    s64 nsecs_o;
    s64 nsecs_x;
    u64 load_avg_o;
    u64 load_avg_x;
};

struct ai_game {
    struct xo_table xo_tlb;
    char turn;
    unsigned cpu;
    unsigned long nsecs_spent;
    enum ai_game_state state;
    struct mutex lock;
    struct work_struct ai_one_work;
    struct work_struct ai_two_work;
    struct work_struct drawboard_work;
};

struct ai_agent {
    ai_alg play;
    char *name;
};

static inline rl_fxp fixed_mul(rl_fxp a, rl_fxp b)
{
    return ((s64) a * b) >> RL_FIXED_SCALE_BITS;
}

static inline rl_fxp fixed_mul_s32(rl_fxp a, s32 b)
{
    b = ((s64) b * RL_FIXED_1);
    return fixed_mul(a, b);
}
