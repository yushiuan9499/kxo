#pragma once

#include "ai-game.h"

#define ITERATIONS 100000

int mcts(struct ai_game *game, char player);
void mcts_init(void);
void free_mcts(void);
