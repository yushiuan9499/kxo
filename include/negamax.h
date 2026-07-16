#pragma once

#include "ai-game.h"

typedef struct {
    int score, move;
} move_t;

void negamax_init(void);
int negamax_predict(struct ai_game *game, char player);
