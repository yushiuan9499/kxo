#pragma once

#include "game.h"

typedef int rl_fxp;
#define RL_FIXED_SCALE_BITS 16
#define RL_FIXED_1 (1 << RL_FIXED_SCALE_BITS)
#define RL_FIXED_MAX CLR_SIGN(~0)
#define RL_FIXED_MIN SET_SIGN(0)
#define AGENT_O (CELL_O - 1)
#define AGENT_X (CELL_X - 1)

/* for training */
#define INITIAL_MUTIPLIER 0x6 /* 0.0001 */
#define LEARNING_RATE 0x51e   /* 0.02 */

/* for Markov decision model */
#define GAMMA 0xfd70 /* 0.99 */
#define REWARD_TRADEOFF RL_FIXED_1

void init_rl_agent(void);

void free_rl_agent(void);

struct ai_game;
int play_rl(struct ai_game *game, char player);

void update_state_value(const int *after_state_hash,
                        const rl_fxp *reward,
                        int steps,
                        char player);
