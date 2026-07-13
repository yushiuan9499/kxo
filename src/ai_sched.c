#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ai-game.h"
#include "ai_sched.h"
#include "game.h"

static const int delay = 10;

DEFINE_SPINLOCK(load_lock);

load_t ai_load[XO_AI_TOT] = {LOAD_1MS, LOAD_1MS, LOAD_1MS};

static int *ai_of_cpu = NULL;
static unsigned long *old_idle_ticks = NULL;

static struct workqueue_struct *ai_workqueue;

struct sched_work {
    struct work_struct work;
    struct work_struct *task;
    int cpu;
};

static void sched_work_func(struct work_struct *work)
{
    struct sched_work *bh = container_of(work, struct sched_work, work);

    /* Wait until the bh->task is ready to be queued */
    while (work_busy(bh->task)) {
        cpu_relax();
    }

    bool ret = queue_work_on(bh->cpu, ai_workqueue, bh->task);
    WARN_ON(!ret);
    kfree(bh);
}

bool resched_self(int cpu, struct work_struct *work)
{
    struct sched_work *bh = kmalloc(sizeof(struct sched_work), GFP_ATOMIC);
    if (!bh) {
        pr_err("kxo: Failed to allocate work_struct for rescheduling\n");
        return false;
    }
    INIT_WORK(&bh->work, sched_work_func);
    bh->task = work;
    bh->cpu = cpu;
    schedule_work(&bh->work);
    return true;
}


#if defined(CONFIG_X86_64) || defined(CONFIG_X86_32)
unsigned long *calculated_capacity = NULL;
static void x86_capacity_init(void)
{
    int cpu;
    u64 hwp_cap;
    unsigned int highest_perf;
    unsigned int weight;
    calculated_capacity =
        kmalloc(sizeof(unsigned long) * num_possible_cpus(), GFP_KERNEL);
    unsigned long max_capacity = 0;

    for_each_possible_cpu(cpu)
    {
        if (rdmsrl_on_cpu(cpu, MSR_HWP_CAPABILITIES, &hwp_cap) == 0) {
            highest_perf = hwp_cap & 0xFF;
        } else {
            goto default_capacity;
        }

        weight = cpumask_weight(topology_sibling_cpumask(cpu));

        calculated_capacity[cpu] = highest_perf * 10ul;

        if (weight > 1) {
            calculated_capacity[cpu] = (calculated_capacity[cpu] * 7) / 10;
        }
        max_capacity = max(max_capacity, calculated_capacity[cpu]);
    }
    if (!max_capacity)
        goto default_capacity;
    for_each_possible_cpu(cpu)
    {
        calculated_capacity[cpu] =
            (calculated_capacity[cpu] << SCHED_CAPACITY_SHIFT) / max_capacity;
    }
    return;
default_capacity:
    for_each_possible_cpu(cpu)
    {
        calculated_capacity[cpu] = 1ul << SCHED_CAPACITY_SHIFT;
    }
}
static void x86_capacity_free(void)
{
    kfree(calculated_capacity);
}

#undef arch_scale_cpu_capacity
#define arch_scale_cpu_capacity(cpu) (calculated_capacity[cpu])
#else
static void x86_capacity_init(void) {}
static void x86_capacity_free(void) {}
#endif

void sched_games(unsigned long unfini, struct ai_game *games)
{
    int cpu;
    load_t ai_load_safe[XO_AI_TOT], ai_load_tot[XO_AI_TOT] = {0},
                                    ai_budget[XO_AI_TOT] = {0};
    long *cpu_budget =
        kmalloc(sizeof(load_t) * num_possible_cpus(), GFP_ATOMIC);
    if (!cpu_budget) {
        pr_err("kxo: Failed to allocate cpu_budget\n");
        return;
    }

    spin_lock_bh(&load_lock);
    memcpy(ai_load_safe, ai_load, sizeof(ai_load_safe));
    spin_unlock_bh(&load_lock);

    unsigned long unfini_copy = unfini;
    int n = hweight32(unfini);
    for (int i = 0; i < n; i++) {
        int id = ffs(unfini_copy) - 1;
        unfini_copy &= ~(1u << id);
        struct ai_game *game = &games[id];
        enum ai_game_state state = READ_ONCE(game->state);
        char turn = READ_ONCE(game->turn);
        int cpu = READ_ONCE(game->cpu);
        smp_rmb();

        int who, other;
        if (state == GAME_READY && turn == 'O') {
            who = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            other = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            ai_load_tot[who] += min(delay * LOAD_1MS, ai_load_safe[who]);
            ai_of_cpu[cpu] = other;
        } else if (state == GAME_READY && turn == 'X') {
            who = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            other = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            ai_load_tot[who] += min(delay * LOAD_1MS, ai_load_safe[who]);
            ai_of_cpu[cpu] = other;
        } else if (state == GAME_BUSY && turn == 'O') {
            who = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            ai_of_cpu[cpu] = who;
        } else if (state == GAME_BUSY && turn == 'X') {
            who = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            ai_of_cpu[cpu] = who;
        }
    }

    bool first_run = !old_idle_ticks;
    if (first_run) {
        old_idle_ticks =
            kmalloc(sizeof(unsigned long) * num_possible_cpus(), GFP_ATOMIC);
        if (!old_idle_ticks) {
            pr_err("kxo: Failed to allocate old_idle_ticks\n");
            kfree(cpu_budget);
            return;
        }
    }
    for_each_possible_cpu(cpu)
    {
        unsigned long idle_ticks = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
        unsigned long delta_idle =
            first_run ? delay * HZ : idle_ticks - old_idle_ticks[cpu];
        old_idle_ticks[cpu] = idle_ticks;

        cpu_budget[cpu] =
            delay * LOAD_1MS * arch_scale_cpu_capacity(cpu) * delta_idle / HZ >>
            SCHED_FIXEDPOINT_SHIFT;
        if (!cpu_online(cpu))
            cpu_budget[cpu] = 0;

        if (ai_of_cpu[cpu] != -1)
            ai_budget[ai_of_cpu[cpu]] += cpu_budget[cpu];
    }

    int best_cpu[XO_AI_TOT + 1];
    long max_budget[XO_AI_TOT + 1];
    for (int i = 0; i < XO_AI_TOT + 1; i++) {
        best_cpu[i] = -1;
        max_budget[i] = -1;
    }
    for_each_online_cpu(cpu)
    {
        if (ai_of_cpu[cpu] != -1) {
            int who = ai_of_cpu[cpu];
            if (cpu_budget[cpu] > max_budget[who]) {
                max_budget[who] = cpu_budget[cpu];
                best_cpu[who] = cpu;
            }
        } else {
            if (cpu_budget[cpu] > max_budget[XO_AI_TOT]) {
                max_budget[XO_AI_TOT] = cpu_budget[cpu];
                best_cpu[XO_AI_TOT] = cpu;
            }
        }
    }

    unsigned long busy = 0;
    for (int i = 0; i < n; i++) {
        int id = ffs(unfini) - 1;
        unfini &= ~(1u << id);
        struct ai_game *game = &games[id];
        enum ai_game_state state = READ_ONCE(game->state);
        char turn = READ_ONCE(game->turn);
        smp_rmb();

        if (state == GAME_READY) {
            int who, from;
            int cpu;

            if (turn == 'O') {
                who = XO_ATTR_AI_ALG(game->xo_tlb.attr) % XO_AI_TOT;
            } else {
                who = (XO_ATTR_AI_ALG(game->xo_tlb.attr) >> 2) % XO_AI_TOT;
            }

            if (best_cpu[who] != -1 && max_budget[who] > delay * LOAD_1MS / 2) {
                cpu = best_cpu[who];
                from = who;
            } else if (best_cpu[XO_AI_TOT] != -1) {
                cpu = best_cpu[XO_AI_TOT];
                from = XO_AI_TOT;
            } else {
                int best_who = who;
                long best_budget = max_budget[who];
                for (int i = 1; i < XO_AI_TOT; i++) {
                    int other = (who + i) % XO_AI_TOT;
                    /* If that ai has no sufficient budget, don't steal its cpu
                     */
                    if (best_cpu[other] == -1)
                        continue;
                    if (best_cpu[best_who] == -1) {
                        best_who = other;
                        best_budget = max_budget[other] * 2 / 3;
                        continue;
                    }
                    if (ai_budget[other] * ai_load_tot[who] / 2 <
                        ai_budget[who] * ai_load_tot[other])
                        continue;
                    if (max_budget[other] * 2 / 3 >= best_budget) {
                        best_budget = max_budget[other] * 2 / 3;
                        best_who = other;
                    }
                }
                from = best_who;
                cpu = best_cpu[best_who];
            }

            WRITE_ONCE(game->cpu, cpu);
            WRITE_ONCE(game->state, GAME_BUSY);
            smp_wmb();

            struct work_struct *work =
                (turn == 'O') ? &game->ai_one_work : &game->ai_two_work;

            queue_work_on(cpu, ai_workqueue, work);

            ai_of_cpu[cpu] = who;
            cpu_budget[cpu] =
                max(0l, cpu_budget[cpu] - (long) ai_load_safe[who]);
            if (from != XO_AI_TOT)
                ai_budget[from] -= max_budget[from];
            ai_budget[who] += max_budget[from];

            if (who != from && cpu_budget[cpu] > max_budget[who]) {
                max_budget[who] = cpu_budget[cpu];
                best_cpu[who] = cpu;
            }

            best_cpu[from] = -1;
            max_budget[from] = -1;
            for_each_online_cpu(cpu)
            {
                if (ai_of_cpu[cpu] == who &&
                    cpu_budget[cpu] > max_budget[from]) {
                    max_budget[from] = cpu_budget[cpu];
                    best_cpu[from] = cpu;
                }
            }

        } else if (state == GAME_BUSY) {
            busy |= (1u << id);
        }
    }
    kfree(cpu_budget);
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

    x86_capacity_init();

out:
    return ret;
error_ai_of_cpu:
    destroy_workqueue(ai_workqueue);
    goto out;
}

void free_ai_sched(void)
{
    flush_workqueue(ai_workqueue);
    destroy_workqueue(ai_workqueue);
    kfree(ai_of_cpu);
    kfree(old_idle_ticks);
    x86_capacity_free();
}
