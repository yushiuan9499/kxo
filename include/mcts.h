#pragma once

#include "ai-game.h"

#define ITERATIONS_PER_ROUND 25000
#define ROUNDS 4

int mcts(struct ai_game *game, char player);
void mcts_init(void);
void free_mcts(void);
