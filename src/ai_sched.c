#include <linux/bitmap.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ai-game.h"
#include "ai_sched.h"
#include "game.h"

static const int delay = 100;

DEFINE_SPINLOCK(load_lock);

u64 ai_load_ns[XO_AI_TOT] = {LOAD_1MS, LOAD_1MS, LOAD_1MS};

DEFINE_PER_CPU(unsigned, work_done) = 0;
DEFINE_PER_CPU(ktime_t, done_time);
unsigned *work_queued = NULL;

#define XO_AI_NONE XO_AI_TOT
static int *ai_of_cpu = NULL;
static long *cpu_budget = NULL;
static unsigned long *idle_cpu_map[XO_AI_TOT + 1] = {NULL, NULL, NULL, NULL};

static struct workqueue_struct *ai_workqueue;

void sched_games(unsigned long unfini, struct ai_game *games)
{
    int cpu;
    u64 ai_load_safe[XO_AI_TOT], ai_load_tot[XO_AI_TOT] = {0},
                                 ai_budget[XO_AI_TOT] = {0};
    int ai_cnt[XO_AI_TOT] = {0};
    spin_lock_bh(&load_lock);
    memcpy(ai_load_safe, ai_load_ns, sizeof(ai_load_safe));
    spin_unlock_bh(&load_lock);

    pr_info("kxo: unfini: 0x%lx\n", unfini);
    unsigned long unfini_copy = unfini;
    int n = hweight32(unfini);
    for (int i = 0; i < n; i++) {
        int id = ffs(unfini_copy) - 1;
        unfini_copy &= ~(1u << id);
        struct ai_game *game = &games[id];
        enum ai_game_state state = READ_ONCE(game->state);
        char turn = READ_ONCE(game->turn);
        smp_rmb();

        int who;
        if (state == GAME_READY) {
            if (turn == 'O')
                who = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            else
                who = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            ai_load_tot[who] += min(delay * LOAD_1MS, ai_load_safe[who]);
            ai_cnt[who]++;
        }
    }

    for (int i = 0; i < XO_AI_TOT + 1; i++) {
        bitmap_clear(idle_cpu_map[i], 0, num_possible_cpus());
    }
    for_each_online_cpu(cpu)
    {
        cpu_budget[cpu] =
            min(delay * LOAD_1MS, cpu_budget[cpu] + delay * LOAD_1MS);

        int who = ai_of_cpu[cpu];
        bool is_idle = (per_cpu(work_done, cpu) == work_queued[cpu]);
        if (is_idle) {
            bitmap_set(idle_cpu_map[who], cpu, 1);
            cpu_budget[cpu] = delay * LOAD_1MS;
        }
        pr_info("kxo: CPU#%d queued %u, done %u, budget: %ld\n", cpu,
                work_queued[cpu], per_cpu(work_done, cpu), cpu_budget[cpu]);

        if (who == XO_AI_NONE)
            continue;
        ai_budget[who] += max(0, cpu_budget[cpu]);
        if (cpu_budget[cpu] == delay * LOAD_1MS) {
            ai_cnt[who]--;
        }
    }
    for (int i = 0; i < XO_AI_TOT; i++) {
        while (ai_cnt[i] > 0 && ai_budget[i] < 2 * ai_load_tot[i]) {
            int idle_cpu =
                find_first_bit(idle_cpu_map[XO_AI_NONE], num_possible_cpus());
            if (idle_cpu == num_possible_cpus())
                break;
            bitmap_clear(idle_cpu_map[XO_AI_NONE], idle_cpu, 1);
            bitmap_set(idle_cpu_map[i], idle_cpu, 1);
            ai_budget[i] += delay * LOAD_1MS;
            ai_cnt[i]--;
        }
        for (int j = 1; j < XO_AI_TOT; j++) {
            int other = (i + j) % XO_AI_TOT;
            while (ai_cnt[i] > 0 && ai_budget[i] < 2 * ai_load_tot[i]) {
                if (ai_cnt[other] > 0 && ai_budget[other] - delay * LOAD_1MS <
                                             4 * ai_load_tot[other])
                    break;
                int idle_cpu =
                    find_first_bit(idle_cpu_map[other], num_possible_cpus());
                if (idle_cpu == num_possible_cpus())
                    break;
                bitmap_clear(idle_cpu_map[other], idle_cpu, 1);
                bitmap_set(idle_cpu_map[i], idle_cpu, 1);
                ai_budget[i] += delay * LOAD_1MS;
                ai_budget[other] -= delay * LOAD_1MS;
                ai_cnt[i]--;
                ai_cnt[other]++;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        int id = ffs(unfini) - 1;
        unfini &= ~(1u << id);
        struct ai_game *game = &games[id];
        enum ai_game_state state = READ_ONCE(game->state);
        char turn = READ_ONCE(game->turn);
        smp_rmb();

        if (state == GAME_READY) {
            int who, cpu, best_cpu = -1;

            if (turn == 'O') {
                who = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            } else {
                who = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            }

            best_cpu = find_first_bit(idle_cpu_map[who], num_possible_cpus());
            if (best_cpu != num_possible_cpus()) {
                bitmap_clear(idle_cpu_map[who], best_cpu, 1);
            } else {
                long this_max_budget = LONG_MIN, other_max_budget = LONG_MIN;
                int other_best_cpu = -1;
                best_cpu = -1;
                for_each_online_cpu(cpu)
                {
                    if (ai_of_cpu[cpu] == who &&
                        cpu_budget[cpu] > this_max_budget) {
                        best_cpu = cpu;
                        this_max_budget = cpu_budget[cpu];
                    } else if (ai_of_cpu[cpu] != who &&
                               cpu_budget[cpu] > other_max_budget &&
                               cpu_budget[cpu] <= ai_load_safe[who]) {
                        int other = ai_of_cpu[cpu];
                        if (bitmap_read(idle_cpu_map[other], cpu, 1) == 1)
                            continue;
                        if (other != XO_AI_NONE &&
                            ai_load_tot[other] < ai_budget[other] * 3 / 2)
                            continue;
                        other_max_budget = cpu_budget[cpu];
                        other_best_cpu = cpu;
                    }
                }
                if (best_cpu == -1) {
                    best_cpu = other_best_cpu;
                } else if (other_best_cpu != -1) {
                    if (this_max_budget < delay * LOAD_1MS / 2 &&
                        other_max_budget > this_max_budget) {
                        best_cpu = other_best_cpu;
                    }
                }
            }

            WRITE_ONCE(game->state, GAME_BUSY);
            smp_wmb();

            struct work_struct *work =
                (turn == 'O') ? &game->ai_one_work : &game->ai_two_work;

            queue_work_on(best_cpu, ai_workqueue, work);
            schedule_work(&game->drawboard_work);
            work_queued[best_cpu]++;

            if (ai_of_cpu[best_cpu] != XO_AI_NONE) {
                ai_budget[ai_of_cpu[best_cpu]] -=
                    max(0ll, cpu_budget[best_cpu]);
            }
            ai_budget[who] += max(0ll, cpu_budget[best_cpu]);
            ai_of_cpu[best_cpu] = who;
            cpu_budget[best_cpu] = cpu_budget[cpu] - (long) ai_load_safe[who];
        }
    }
}

int init_ai_sched(void)
{
    int ret = 0;
    ai_workqueue = alloc_workqueue("kxo_ai", WQ_PERCPU, WQ_MAX_ACTIVE);
    if (!ai_workqueue)
        return -ENOMEM;

    ai_of_cpu = kmalloc(sizeof(int) * num_possible_cpus(), GFP_KERNEL);
    if (!ai_of_cpu) {
        ret = -ENOMEM;
        goto error_ai_of_cpu;
    }
    int cpu;
    for_each_possible_cpu(cpu) ai_of_cpu[cpu] = XO_AI_NONE;

    work_queued = kzalloc(sizeof(unsigned) * num_possible_cpus(), GFP_KERNEL);
    if (!work_queued) {
        ret = -ENOMEM;
        goto error_work_queued;
    }

    cpu_budget =
        kmalloc(sizeof(unsigned long) * num_possible_cpus(), GFP_KERNEL);
    if (!cpu_budget) {
        ret = -ENOMEM;
        goto error_cpu_budget;
    }

    int i;
    for (i = 0; i < XO_AI_TOT + 1; i++) {
        idle_cpu_map[i] =
            kmalloc(sizeof(unsigned long) * BITS_TO_LONGS(num_possible_cpus()),
                    GFP_KERNEL);
        if (!idle_cpu_map[i]) {
            ret = -ENOMEM;
            goto error_idle_cpu_map;
        }
    }
out:
    return ret;
error_idle_cpu_map:
    for (i--; i >= 0; i--) {
        kfree(idle_cpu_map[i]);
    }
    kfree(cpu_budget);
error_cpu_budget:
    kfree(work_queued);
error_work_queued:
    kfree(ai_of_cpu);
error_ai_of_cpu:
    destroy_workqueue(ai_workqueue);
    goto out;
}

void free_ai_sched(void)
{
    flush_workqueue(ai_workqueue);
    destroy_workqueue(ai_workqueue);
    kfree(ai_of_cpu);
    kfree(cpu_budget);
}
