#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>

#include "ai-game.h"
#include "ai_sched.h"
#include "game.h"
#include "negamax.h"
#include "util.h"
#include "zobrist.h"

#define MAX_SEARCH_DEPTH 6

static int history_score_sum[N_GRIDS];
static int history_count[N_GRIDS];

static u64 hash_value;

#define HT_BITS 4
static DEFINE_SPINLOCK(hash_lock);
static DECLARE_HASHTABLE(hash_table, HT_BITS);
struct negamax_state {
    struct ai_game *key;
    int depth;
    struct hlist_node node;
};

static int cmp_moves(const void *a, const void *b)
{
    const int *_a = (int *) a, *_b = (int *) b;
    int score_a = 0, score_b = 0;

    if (history_count[*_a])
        score_a = history_score_sum[*_a] / history_count[*_a];
    if (history_count[*_b])
        score_b = history_score_sum[*_b] / history_count[*_b];
    return score_b - score_a;
}

static move_t negamax(unsigned int table,
                      int depth,
                      char player,
                      int alpha,
                      int beta)
{
    if (check_win(table) != CELL_EMPTY || depth == 0) {
        move_t result = {get_score(table, player), -1};
        return result;
    }
    int cached_score, cached_move;
    if (zobrist_get(hash_value, &cached_score, &cached_move))
        return (move_t) {.score = cached_score, .move = cached_move};

    int score;
    move_t best_move = {-10000, -1};
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;

    sort(moves, n_moves, sizeof(int), cmp_moves, NULL);

    for (int i = 0; i < n_moves; i++) {
        table = VAL_SET_CELL(table, moves[i], player);

        hash_value ^= zobrist_table[moves[i]][player == CELL_X];
        if (!i)
            score = -negamax(table, depth - 1, player ^ CELL_O ^ CELL_X, -beta,
                             -alpha)
                         .score;
        else {
            score = -negamax(table, depth - 1, player ^ CELL_O ^ CELL_X,
                             -alpha - 1, -alpha)
                         .score;
            if (alpha < score && score < beta)
                score = -negamax(table, depth - 1, player ^ CELL_O ^ CELL_X,
                                 -beta, -score)
                             .score;
        }
        history_count[moves[i]]++;
        history_score_sum[moves[i]] += score;
        if (score > best_move.score) {
            best_move.score = score;
            best_move.move = moves[i];
        }
        table = VAL_SET_CELL(table, moves[i], CELL_EMPTY);
        hash_value ^= zobrist_table[moves[i]][player == CELL_X];
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    kfree((char *) moves);
    zobrist_put(hash_value, best_move.score, best_move.move);
    return best_move;
}

void negamax_init(void)
{
    zobrist_init();
    hash_value = 0;
}

static struct negamax_state *find_negamax_state(struct ai_game *game)
{
    struct negamax_state *state = NULL;
    u32 key = hash_ptr(game, HT_BITS);
    hash_for_each_possible(hash_table, state, node, key)
    {
        if (state->key == game)
            return state;
    }
    return NULL;
}


int negamax_predict(struct ai_game *game, char player)
{
    unsigned int table = game->xo_tlb.table;
    int old_cpu = READ_ONCE(game->cpu);
    int start_depth = 2;
    memset(history_score_sum, 0, sizeof(history_score_sum));
    memset(history_count, 0, sizeof(history_count));
    struct negamax_state *state = find_negamax_state(game);
    if (state) {
        start_depth = state->depth;
    }
    move_t result;
    for (int depth = start_depth; depth <= MAX_SEARCH_DEPTH; depth += 2) {
        if (check_sched(game, &old_cpu)) {
            if (!state) {
                state = kzalloc(sizeof(struct negamax_state), GFP_KERNEL);
                if (!state)
                    return -1;
                state->key = game;
                spin_lock_bh(&hash_lock);
                hash_add(hash_table, &state->node, hash_ptr(game, HT_BITS));
                spin_unlock_bh(&hash_lock);
            }
            state->depth = depth;
            return AI_RESCHED;
        }
        result = negamax(table, depth, player, -100000, 100000);
        zobrist_clear();
    }
    if (state) {
        spin_lock_bh(&hash_lock);
        hash_del(&state->node);
        spin_unlock_bh(&hash_lock);
        kfree(state);
    }
    return result.move;
}

void free_negamax(void)
{
    struct negamax_state *state;
    struct hlist_node *tmp;
    int i;
    spin_lock_bh(&hash_lock);
    hash_for_each_safe(hash_table, i, tmp, state, node)
    {
        hash_del(&state->node);
        kfree(state);
    }
    spin_unlock_bh(&hash_lock);
}
