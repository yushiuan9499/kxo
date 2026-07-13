#include <linux/random.h>
#include <linux/spinlock_types.h>

#include "ai-game.h"
#include "rl-private.h"
#include "rl.h"

static DEFINE_SPINLOCK(rl_lock);

int play_rl(struct ai_game *game, char player)
{
    int max_act = -1;
    rl_fxp max_q = RL_FIXED_MIN;
    int candidate_count = 1;
    u8 id = player - 1;
    unsigned long flags;
    unsigned int table = game->xo_tlb.table;
    spin_lock_irqsave(&rl_lock, flags);
    for_each_empty_grid(i, table)
    {
        unsigned int next = VAL_SET_CELL(table, i, player);
        rl_fxp new_q = find_rl_state(next)[id];
        if (new_q == max_q) {
            ++candidate_count;
            if (get_random_u32() % candidate_count == 0)
                max_act = i;
        } else if (new_q > max_q) {
            candidate_count = 1;
            max_q = new_q;
            max_act = i;
        }
    }
    spin_unlock_irqrestore(&rl_lock, flags);
    return max_act;
}

/* player assume always AGENT_O or AGENT_X */
static inline rl_fxp step_update_state_value(int after_state_hash,
                                             rl_fxp reward,
                                             rl_fxp next,
                                             u8 player)
{
    rl_fxp curr = reward - fixed_mul(GAMMA, next);
    rl_fxp *scores = find_rl_state(after_state_hash);
    scores[player] = fixed_mul((RL_FIXED_1 - LEARNING_RATE), scores[player]) +
                     fixed_mul(LEARNING_RATE, curr);
    return scores[player];
}

void update_state_value(const int *after_state_hash,
                        const rl_fxp *reward,
                        int steps,
                        char player)
{
    unsigned long flags;
    spin_lock_irqsave(&rl_lock, flags);
    rl_fxp next = 0;
    for (int j = steps - 1; j >= 0; j--)
        if (after_state_hash[j])
            next = step_update_state_value(after_state_hash[j], reward[j], next,
                                           player - 1);
    spin_unlock_irqrestore(&rl_lock, flags);
}
