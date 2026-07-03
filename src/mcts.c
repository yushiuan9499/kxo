#include <linux/slab.h>
#include <linux/string.h>

#include "game.h"
#include "mcts.h"
#include "util.h"

struct node {
    int move;
    char player;
    int n_visits;
    u64 score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

static DEFINE_SPINLOCK(xoro_lock);
static struct mcts_info mcts_obj;

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
    char current_player = player;
    uint32_t temp_table = table;
    spin_lock_bh(&xoro_lock);
    xoro_jump(&(mcts_obj.xoro_obj));
    spin_unlock_bh(&xoro_lock);
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            kfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        spin_lock_bh(&xoro_lock);
        u64 rand_val = xoro_next(&(mcts_obj.xoro_obj));
        spin_unlock_bh(&xoro_lock);
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

int mcts(uint32_t table, char player)
{
    char win;
    struct node *root = new_node(-1, player, NULL);
    if (!root)
        return -1;
    for (int i = 0; i < ITERATIONS; i++) {
        if (READ_ONCE(kxo_stop_work))
            break;
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
    return best_move;
}

void mcts_init(void)
{
    xoro_init(&(mcts_obj.xoro_obj));
}
