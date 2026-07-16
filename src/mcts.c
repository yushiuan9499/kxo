#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ai-game.h"
#include "ai_sched.h"
#include "game.h"
#include "mcts.h"
#include "util.h"
#include "xoroshiro.h"

struct node {
    int move;
    char player;
    int n_visits;
    u64 score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

DEFINE_PER_CPU(struct state_array, xoro_obj);

#define HT_BITS 4
static DEFINE_SPINLOCK(hash_lock);
static DECLARE_HASHTABLE(hash_table, HT_BITS);
struct mcts_state {
    struct ai_game *key;
    struct node *root;
    int iter;
    struct hlist_node node;
};

static struct node *new_node(int move, char player, struct node *parent)
{
    struct node *node = kzalloc(sizeof(struct node), GFP_KERNEL);
    if (!node)
        return NULL;
    node->move = move;
    node->player = player;
    node->parent = parent;
    return node;
}

static void free_node(struct node *node)
{
    for (int i = 0; i < N_GRIDS; i++)
        if (node->children[i])
            free_node(node->children[i]);
    kfree(node);
}

static fixed_point_t fixed_sqrt(fixed_point_t x)
{
    if (!x || x == (1U << FIXED_SCALE_BITS))
        return x;

    fixed_point_t s = 0U;
    for (int i = (31 - __builtin_clz(x | 1)); i >= 0; i--) {
        fixed_point_t t = (1U << i);
        u64 candidate = (u64) s + t;
        if (((candidate * candidate) >> FIXED_SCALE_BITS) <= x)
            s += t;
    }
    return s;
}

static fixed_point_t fixed_log(fixed_point_t v)
{
    if (!v || v == (1U << FIXED_SCALE_BITS))
        return 0;

    fixed_point_t numerator = (v - (1U << FIXED_SCALE_BITS));
    int neg = 0;
    if (GET_SIGN(numerator)) {
        neg = 1;
        numerator = CLR_SIGN(numerator);
        numerator = (1U << 31) - numerator;
    }

    fixed_point_t y = ((u64) numerator << FIXED_SCALE_BITS) /
                      ((u64) v + (1U << FIXED_SCALE_BITS));

    fixed_point_t ans = 0U;
    for (unsigned i = 1; i < 20; i += 2) {
        fixed_point_t z = (1U << FIXED_SCALE_BITS);
        for (int j = 0; j < i; j++) {
            z = ((u64) z * y) >> FIXED_SCALE_BITS;
        }
        z = ((u64) z << FIXED_SCALE_BITS) / (i << FIXED_SCALE_BITS);

        ans += z;
    }
    ans <<= 1;
    ans = neg ? SET_SIGN(ans) : ans;
    return ans;
}

#define EXPLORATION_FACTOR fixed_sqrt(1U << (FIXED_SCALE_BITS + 1))

static inline fixed_point_t uct_score(int n_total, int n_visits, u64 score)
{
    if (n_visits == 0)
        return FIXED_MAX;

    fixed_point_t result = (fixed_point_t) (score / n_visits);
    fixed_point_t log_val = fixed_log(
        (n_total < 65536) ? (n_total << FIXED_SCALE_BITS) : FIXED_MAX);
    fixed_point_t tmp =
        ((u64) EXPLORATION_FACTOR * fixed_sqrt(log_val / n_visits)) >>
        FIXED_SCALE_BITS;
    return result + tmp;
}

static struct node *select_move(struct node *node)
{
    struct node *best_node = NULL;
    fixed_point_t best_score = 0U;
    for (int i = 0; i < N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        fixed_point_t score =
            uct_score(node->n_visits, node->children[i]->n_visits,
                      node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static fixed_point_t simulate(uint32_t table, char player)
{
    struct state_array *this_xoro_obj;
    char current_player = player;
    uint32_t temp_table = table;
    int cpu;

    cpu = get_cpu();
    this_xoro_obj = per_cpu_ptr(&xoro_obj, cpu);
    xoro_jump(this_xoro_obj);
    put_cpu();

    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            kfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;

        cpu = get_cpu();
        this_xoro_obj = per_cpu_ptr(&xoro_obj, cpu);
        u64 rand_val = xoro_next(this_xoro_obj);
        put_cpu();
        int move = moves[rand_val % n_moves];

        kfree(moves);
        temp_table = VAL_SET_CELL(temp_table, move, current_player);
        char win;
        if ((win = check_win(temp_table)) != CELL_EMPTY)
            return calculate_win_value(win, player);
        current_player ^= CELL_O ^ CELL_X;
    }
    return (fixed_point_t) (1UL << (FIXED_SCALE_BITS - 1));
}

static void backpropagate(struct node *node, fixed_point_t score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = (1U << FIXED_SCALE_BITS) - score;
    }
}

static int expand(struct node *node, uint32_t table)
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] =
            new_node(moves[i], node->player ^ CELL_O ^ CELL_X, node);
        if (!node->children[i]) {
            kfree(moves);
            return i;
        }
    }
    kfree(moves);
    return n_moves;
}

static struct mcts_state *find_mcts_state(struct ai_game *game)
{
    struct mcts_state *state = NULL;
    u32 key = hash_ptr(game, HT_BITS);
    hash_for_each_possible(hash_table, state, node, key)
    {
        if (state->key == game)
            return state;
    }
    return NULL;
}

int mcts(struct ai_game *game, char player)
{
    char win;
    unsigned int table = game->xo_tlb.table;
    int old_cpu = READ_ONCE(game->cpu);

    struct node *root;
    int iter_start;
    spin_lock_bh(&hash_lock);
    struct mcts_state *state = find_mcts_state(game);
    spin_unlock_bh(&hash_lock);
    if (!state) {
        root = new_node(-1, player, NULL);
        if (!root)
            return -1;
        iter_start = 0;
    } else {
        root = state->root;
        iter_start = state->iter;
    }

    for (int i = iter_start; i < ITERATIONS; i++) {
        if (READ_ONCE(kxo_stop_work))
            break;
        if (check_sched(game, &old_cpu)) {
            if (!state) {
                state = kzalloc(sizeof(struct mcts_state), GFP_KERNEL);
                if (!state) {
                    free_node(root);
                    return -1;
                }
                state->key = game;
                state->root = root;
                spin_lock_bh(&hash_lock);
                hash_add(hash_table, &state->node, hash_ptr(game, HT_BITS));
                spin_unlock_bh(&hash_lock);
            }
            state->iter = i;
            return AI_RESCHED;
        }
        struct node *node = root;
        uint32_t temp_table = table;
        while (1) {
            if ((win = check_win(temp_table)) != CELL_EMPTY) {
                fixed_point_t score =
                    calculate_win_value(win, node->player ^ CELL_O ^ CELL_X);
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                fixed_point_t score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (node->children[0] == NULL)
                expand(node, temp_table);
            node = select_move(node);
            if (!node) {
                free_node(root);
                return -1;
            }
            temp_table = VAL_SET_CELL(temp_table, node->move,
                                      node->player ^ CELL_O ^ CELL_X);
        }
    }
    struct node *best_node = root;
    int most_visits = -1;
    for (int i = 0; i < N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > most_visits) {
            most_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    int best_move = best_node->move;
    free_node(root);
    if (state) {
        spin_lock_bh(&hash_lock);
        hash_del(&state->node);
        spin_unlock_bh(&hash_lock);
        kfree(state);
    }
    return best_move;
}

static void this_cpu_init(void *data)
{
    struct state_array *xoro_obj_cpu = this_cpu_ptr(&xoro_obj);
    xoro_init(xoro_obj_cpu);
}

void mcts_init(void)
{
    int cpu;
    for_each_online_cpu(cpu)
    {
        int err = smp_call_function_single(cpu, this_cpu_init, NULL, 1);
        if (err) {
            pr_err("kxo: failed to initialize xoro_obj for CPU %d\n", cpu);
        }
    }
}

void free_mcts(void)
{
    struct mcts_state *state = NULL;
    struct hlist_node *tmp;
    int i;
    spin_lock_bh(&hash_lock);
    hash_for_each_safe(hash_table, i, tmp, state, node)
    {
        hash_del(&state->node);
        free_node(state->root);
        kfree(state);
    }
    spin_unlock_bh(&hash_lock);
}
