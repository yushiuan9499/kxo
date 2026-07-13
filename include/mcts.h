#pragma once

#include "ai-game.h"
#include "xoroshiro.h"

#define ITERATIONS 100000

struct mcts_info {
    struct state_array xoro_obj;
    int nr_active_nodes;
};

int mcts(struct ai_game *game, char player);
void mcts_init(void);
void free_mcts(void);
