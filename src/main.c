/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */

#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/sched/loadavg.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "ai-game.h"
#include "ai_sched.h"
#include "mcts.h"
#include "negamax.h"
#include "rl.h"
#include "util.h"
#include "zobrist.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine");

/* Macro DECLARE_TASKLET_OLD exists for compatibility.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define del_timer_sync timer_delete_sync
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int avg_period = 1000;
static int delay = 10; /* time (in ms) to generate an event */

/* Declare kernel module attribute for sysfs */

struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

static struct kxo_attr attr_obj;
bool kxo_stop_work;
static struct ai_game games[N_GAMES];
static struct ai_avg ai_avgs[N_GAMES];
static struct xo_avg xo_avgs[N_GAMES];
static struct ai_agent agents[XO_AI_TOT] = {
    [XO_AI_MCTS] = {.play = mcts, .name = "MCTS"},
    [XO_AI_NEGAMAX] = {.play = negamax_predict, .name = "NEGAMAX"},
    [XO_AI_RL] = {.play = play_rl, .name = "RL"},
};

static int episode_moves[N_GAMES][N_GRIDS] = {0};
static rl_fxp reward[N_GAMES][N_GRIDS] = {0};

static void clear_episode(int id)
{
    memset(episode_moves[id], 0, sizeof(episode_moves[id]));
    memset(reward[id], 0, sizeof(reward[id]));
}

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 7, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    char d, r, e;
    if (sscanf(buf, "%c %c %c", &d, &r, &e) != 3)
        return -EINVAL;

    write_lock(&attr_obj.lock);
    attr_obj.display = d;
    attr_obj.resume = r;
    WRITE_ONCE(attr_obj.end, e);
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timers */
static struct timer_list timer;
static struct timer_list loadavg_timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;
static struct cdev kxo_cdev;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);
static DEFINE_MUTEX(consumer_lock);
static DEFINE_SPINLOCK(avg_lock);
static DEFINE_MUTEX(negamax_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Insert a game table into the kfifo buffer */
static void produce_board(const struct xo_table *xo_tlb)
{
    size_t sz = sizeof(struct xo_table);
    unsigned int len = kfifo_in(&rx_fifo, (unsigned char *) xo_tlb, sz);
    if (unlikely(len < sz))
        pr_warn_ratelimited("%s: %zu bytes dropped\n", __func__, sz - len);

    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo));
}

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void drawboard_work_func(struct work_struct *w)
{
    int cpu;
    int id = -1;
    struct ai_game *game = container_of(w, struct ai_game, drawboard_work);

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    /* Store data to the kfifo buffer */
    mutex_lock(&game->lock);
    if (game->state == GAME_BUSY) {
        mutex_unlock(&game->lock);
        return;
    }
    id = XO_ATTR_ID(game->xo_tlb.attr);
    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] game-%d %s\n", cpu, id, __func__);
    put_cpu();
    mutex_lock(&consumer_lock);
    produce_board(&game->xo_tlb);
    mutex_unlock(&consumer_lock);
    mutex_unlock(&game->lock);

    wake_up_interruptible(&rx_wait);
}

static bool kxo_shutting_down(void)
{
    return READ_ONCE(kxo_stop_work) || READ_ONCE(attr_obj.end) != '0';
}

static void kxo_set_work_stop(bool stop)
{
    WRITE_ONCE(kxo_stop_work, stop);
}

static int play_agent_move(int who, struct ai_game *game, char player)
{
    int move;

    if (kxo_shutting_down())
        return -1;

    switch (who) {
    case XO_AI_MCTS:
        move = agents[who].play(game, player);
        break;
    case XO_AI_NEGAMAX:
        mutex_lock(&negamax_lock);
        move = kxo_shutting_down() ? -1 : agents[who].play(game, player);
        mutex_unlock(&negamax_lock);
        break;
    default:
        move = agents[who].play(game, player);
        break;
    }

    return move;
}

static void ai_one_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;
    struct ai_game *game = container_of(w, struct ai_game, ai_one_work);
    struct xo_table *xo_tlb = &game->xo_tlb;
    int attr = xo_tlb->attr;
    unsigned int table = xo_tlb->table;
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Bail out quickly when the module is shutting down so that
     * flush_workqueue() in kxo_exit() does not block for seconds
     * waiting for a 100k-iteration MCTS computation to finish.
     */
    if (kxo_shutting_down()) {
        WRITE_ONCE(game->state, GAME_READY);
        return;
    }

    int id = XO_ATTR_ID(attr);
    int steps = XO_ATTR_STEPS(attr);
    pr_info("kxo: game-%d start doing %s\n", id, __func__);
    tv_start = ktime_get();
    mutex_lock(&game->lock);
    int move;
    int who = XO_ATTR_AI_ALG(attr) % XO_AI_TOT;
    bool is_rl = who == XO_AI_RL;
    struct ai_agent *agent = &agents[who];
    pr_debug("[one]: id=%d, alg=%d\n", id, who);
    WRITE_ONCE(move, play_agent_move(who, game, CELL_O));
    smp_mb();

    if (move == AI_RESCHED) {
        WRITE_ONCE(game->state, GAME_RESCHED);
        WRITE_ONCE(game->cpu, clear_force(game->cpu));
        smp_wmb();
        resched_self(game->cpu, &game->ai_one_work);
        mutex_unlock(&game->lock);
        return;
    }

    if (move != -1) {
        WRITE_ONCE(xo_tlb->table, VAL_SET_CELL(table, move, CELL_O));
        WRITE_ONCE(xo_tlb->moves, SET_RECORD_CELL(xo_tlb->moves, move, steps));

        if (is_rl) {
            table = xo_tlb->table;
            u8 win = check_win(table);
            episode_moves[id][steps] = table;
            rl_fxp score = fixed_mul_s32((RL_FIXED_1 - REWARD_TRADEOFF),
                                         get_score(table, CELL_O));
            reward[id][steps] = score + calculate_win_value(win, CELL_O);
        }
        WRITE_ONCE(xo_tlb->attr, XO_SET_ATTR_STEPS(attr, steps + 1));
        pr_debug("[one]move: [%d, %x, %lx]\n", steps, move, xo_tlb->moves);
        WRITE_ONCE(game->turn, 'X');
    }

    WRITE_ONCE(game->state, GAME_READY);
    smp_wmb();
    mutex_unlock(&game->lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    spin_lock_bh(&avg_lock);
    ai_avgs[id].nsecs_o += nsecs;
    spin_unlock_bh(&avg_lock);

    commit_load(smp_processor_id(), who, nsecs);

    pr_info("kxo: [CPU#%d] game-%d %s:%s completed in %llu usec\n",
            smp_processor_id(), id, __func__, agent->name,
            (unsigned long long) nsecs >> 10);
}

static void ai_two_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    struct ai_game *game = container_of(w, struct ai_game, ai_two_work);
    struct xo_table *xo_tlb = &game->xo_tlb;
    unsigned int attr = xo_tlb->attr;
    unsigned int table = xo_tlb->table;
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    if (kxo_shutting_down()) {
        WRITE_ONCE(game->state, GAME_READY);
        return;
    }

    int id = XO_ATTR_ID(attr);
    int steps = XO_ATTR_STEPS(attr);
    pr_info("kxo: game-%d start doing %s\n", id, __func__);
    tv_start = ktime_get();
    mutex_lock(&game->lock);
    int move;
    int who = (XO_ATTR_AI_ALG(attr) >> 2) % XO_AI_TOT;
    bool is_rl = who == XO_AI_RL;
    struct ai_agent *agent = &agents[who];
    pr_debug("[two]: id=%d, alg=%d\n", id, who);
    WRITE_ONCE(move, play_agent_move(who, game, CELL_X));
    smp_mb();

    if (move == AI_RESCHED) {
        WRITE_ONCE(game->state, GAME_RESCHED);
        WRITE_ONCE(game->cpu, clear_force(game->cpu));
        smp_wmb();
        resched_self(game->cpu, &game->ai_two_work);
        mutex_unlock(&game->lock);
        return;
    }

    if (move != -1) {
        WRITE_ONCE(xo_tlb->table, VAL_SET_CELL(table, move, CELL_X));
        WRITE_ONCE(xo_tlb->moves, SET_RECORD_CELL(xo_tlb->moves, move, steps));

        if (is_rl) {
            table = xo_tlb->table;
            u8 win = check_win(table);
            episode_moves[id][steps] = table;
            rl_fxp score = fixed_mul_s32((RL_FIXED_1 - REWARD_TRADEOFF),
                                         get_score(table, CELL_X));
            reward[id][steps] = score + calculate_win_value(win, CELL_X);
        }
        WRITE_ONCE(xo_tlb->attr, XO_SET_ATTR_STEPS(attr, steps + 1));
        pr_debug("[two]move: [%d, %x, %lx]\n", steps, move, xo_tlb->moves);
        WRITE_ONCE(game->turn, 'O');
    }

    WRITE_ONCE(game->state, GAME_READY);
    smp_wmb();
    mutex_unlock(&game->lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    spin_lock_bh(&avg_lock);
    ai_avgs[id].nsecs_x += nsecs;
    spin_unlock_bh(&avg_lock);

    commit_load(smp_processor_id(), who, nsecs);


    pr_info("kxo: [CPU#%d]  game-%d %s:%s completed in %llu usec\n",
            smp_processor_id(), id, __func__, agent->name,
            (unsigned long long) nsecs >> 10);
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue;

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void game_tasklet_func(unsigned long unfini)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    tv_start = ktime_get();
    sched_games(unfini, games);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(game_tasklet, game_tasklet_func);

static void ai_game(void)
{
    tasklet_schedule(&game_tasklet);
}

static void loadavg_handler(struct timer_list *__timer)
{
    static ktime_t tv_end = 0;
    ktime_t tv_start;
    s64 delta;

    tv_start = ktime_get();
    if (!tv_end) {
        tv_end = tv_start;
        goto leave;
    }

    delta = ktime_to_ns(ktime_sub(tv_start, tv_end));
    tv_end = tv_start;
    spin_lock(&avg_lock);
    for (int i = 0; i < N_GAMES; i++) {
        struct ai_avg *avg = &ai_avgs[i];
        u64 ratio;
        if (avg->nsecs_o) {
            ratio = div64_u64(avg->nsecs_o * FIXED_1, delta);
            pr_debug("kxo: avg->nsecs_o=%llu\n", avg->nsecs_o);
            avg->load_avg_o = calc_load(avg->load_avg_o, EXP_1, ratio);
            avg->nsecs_o = 0;
        }

        if (avg->nsecs_x) {
            ratio = div64_u64(avg->nsecs_x * FIXED_1, delta);
            pr_debug("kxo: avg->nsecs_x=%llu\n", avg->nsecs_x);
            avg->load_avg_x = calc_load(avg->load_avg_x, EXP_1, ratio);
            avg->nsecs_x = 0;
        }

        const u16 x_int = LOAD_INT(avg->load_avg_x);
        const u16 x_frac = LOAD_FRAC(avg->load_avg_x);
        const u16 o_int = LOAD_INT(avg->load_avg_o);
        const u16 o_frac = LOAD_FRAC(avg->load_avg_o);

        xo_avgs[i].avg_x = (x_int << 7) + (x_frac & 0x7f);
        xo_avgs[i].avg_o = (o_int << 7) + (o_frac & 0x7f);

        pr_debug(
            "kxo[%d] avg_x = %d.%02d, avg_o = %d.%02d ! avg_x = %d.%02d, "
            "avg_o = %d.%02d\n",
            i, (xo_avgs[i].avg_x & 0x780) >> 7, xo_avgs[i].avg_x & 0x7f,
            (xo_avgs[i].avg_o & 0x780) >> 7, xo_avgs[i].avg_o & 0x7f, x_int,
            x_frac, o_int, o_frac);
    }
    spin_unlock(&avg_lock);
leave:
    mod_timer(&loadavg_timer, jiffies + msecs_to_jiffies(avg_period));
}

/* Work item for finished-game handling (needs sleeping locks) */
struct finish_work {
    struct work_struct work;
    int game_idx;
    uint8_t win;
};

static struct finish_work finish_works[N_GAMES];

static void finish_game_work_func(struct work_struct *w)
{
    struct finish_work *fw = container_of(w, struct finish_work, work);
    int i = fw->game_idx;
    uint8_t win = fw->win;
    struct ai_game *game = &games[i];
    struct xo_table *xo_tlb = &game->xo_tlb;
    char display, end;

    read_lock(&attr_obj.lock);
    display = attr_obj.display;
    end = attr_obj.end;
    read_unlock(&attr_obj.lock);

    mutex_lock(&game->lock);
    if (display == '1') {
        mutex_lock(&consumer_lock);
        produce_board(xo_tlb);
        mutex_unlock(&consumer_lock);
        wake_up_interruptible(&rx_wait);
    }

    if (end == '0') {
        int steps = XO_ATTR_STEPS(xo_tlb->attr);
        int alg = XO_ATTR_AI_ALG(xo_tlb->attr);
        u32 rnd = get_random_u32();
        unsigned int attr = i; /* preserve game ID */
        attr =
            XO_SET_ATTR_AI_ALG(attr, rnd % XO_AI_TOT, (rnd >> 16) % XO_AI_TOT);
        xo_tlb->attr = attr;
        xo_tlb->table = 0;
        xo_tlb->moves = 0;
        WRITE_ONCE(game->turn, 'O');
        WRITE_ONCE(game->state, GAME_READY);
        if ((win == CELL_O && alg % XO_AI_TOT == XO_AI_RL) ||
            (win == CELL_X && (alg >> 2) % XO_AI_TOT == XO_AI_RL))
            update_state_value(episode_moves[i], reward[i], steps, win);
        clear_episode(i);
    }
    mutex_unlock(&game->lock);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;
    char cell_tlb[] = "OXD";
    unsigned int unfini = 0;

    pr_info("kxo: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();
    for (int i = 0; i < N_GAMES; i++) {
        struct ai_game *game = &games[i];
        struct xo_table *xo_tlb = &game->xo_tlb;
        enum ai_game_state state = READ_ONCE(game->state);
        if (state == GAME_BUSY) {
            unfini |= (1u << i);
            continue;
        }
        if (state == GAME_DONE)
            continue;
        uint8_t win = check_win(xo_tlb->table);
        uint8_t id = XO_ATTR_ID(xo_tlb->attr);
        if (win == CELL_EMPTY) {
            unfini |= (1u << i);
        } else {
            int winner = win - 1;
            if (likely(cell_tlb[winner] != 'D'))
                pr_info("kxo: game-%d %c win!!!\n", id, cell_tlb[winner]);
            else {
                int alg = XO_ATTR_AI_ALG(xo_tlb->attr);
                pr_info("kxo: game-%d %4s vs %-4s Draw!!!\n", id,
                        agents[alg % XO_AI_TOT].name,
                        agents[(alg >> 2) % XO_AI_TOT].name);
            }
            /* Defer sleeping work (mutex, kfifo, RL update) to workqueue */
            WRITE_ONCE(game->state, GAME_DONE);
            finish_works[i].game_idx = i;
            finish_works[i].win = win;
            queue_work(kxo_workqueue, &finish_works[i].work);
        }
    }
    const unsigned int fini = ~unfini & GENMASK(N_GAMES - 1, 0);

    if (unfini && !kxo_shutting_down()) {
        WRITE_ONCE(game_tasklet.data, unfini);
        ai_game();
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    } else if (!kxo_shutting_down() && fini)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));

    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_debug("kxo: unfini=%x fini=%x\n", unfini, fini);
    pr_info("kxo: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

static __poll_t kxo_poll(struct file *filp, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    poll_wait(filp, &rx_wait, wait);

    if (kfifo_len(&rx_fifo))
        mask |= EPOLLRDNORM | EPOLLIN;

    return mask;
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("kxo: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read, kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1) {
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
        mod_timer(&loadavg_timer, jiffies + msecs_to_jiffies(avg_period));
    }
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static long kxo_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = 0;
    if (!arg)
        return -EFAULT;

    switch (cmd) {
    case XO_IO_LDAVG:
        if (copy_to_user((void __user *) arg, xo_avgs, sizeof(xo_avgs)))
            ret = -EFAULT;
        break;
    default:
        ret = -EINVAL;
    }

    return ret;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        kxo_set_work_stop(true);
        del_timer_sync(&timer);
        del_timer_sync(&loadavg_timer);
        tasklet_kill(&game_tasklet);
        flush_workqueue(kxo_workqueue);
        for (int i = 0; i < N_GAMES; i++) {
            mutex_lock(&games[i].lock);
            if (games[i].state == GAME_DONE)
                WRITE_ONCE(games[i].state, GAME_READY);
            mutex_unlock(&games[i].lock);
        }
        fast_buf_clear();
        kxo_set_work_stop(false);
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));
    write_lock(&attr_obj.lock);
    WRITE_ONCE(attr_obj.end, '0');
    attr_obj.display = '1';
    write_unlock(&attr_obj.lock);

    return 0;
}

static const struct file_operations kxo_fops = {
    .owner = THIS_MODULE,
    .read = kxo_read,
    .llseek = noop_llseek,
    .open = kxo_open,
    .unlocked_ioctl = kxo_ioctl,
    .release = kxo_release,
    .poll = kxo_poll,
};

static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_KMLDRV, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kxo_cdev, &kxo_fops);
    ret = cdev_add(&kxo_cdev, dev_id, NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        printk(KERN_ERR "error creating kxo class\n");
        ret = PTR_ERR(kxo_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    struct device *kxo_dev =
        device_create(kxo_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    ret = device_create_file(kxo_dev, &dev_attr_kxo_state);
    if (ret < 0) {
        printk(KERN_ERR "failed to create sysfs file kxo_state\n");
        goto error_device;
    }

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        ret = -ENOMEM;
        goto error_vmalloc;
    }

    /* Create the workqueue */
    kxo_workqueue = alloc_workqueue("kxod", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue) {
        ret = -ENOMEM;
        goto error_workqueue;
    }

    negamax_init();
    mcts_init();
    init_rl_agent();
    fill_win_patterns();
    init_ai_sched();

    for (int i = 0; i < N_GAMES; i++) {
        INIT_WORK(&finish_works[i].work, finish_game_work_func);
        unsigned int attr;
        u32 rnd = get_random_u32();
        struct ai_game *game = &games[i];
        game->xo_tlb.table = 0;
        attr = i; /* set game ID in attr */
        attr =
            XO_SET_ATTR_AI_ALG(attr, rnd % XO_AI_TOT, (rnd >> 16) % XO_AI_TOT);
        game->xo_tlb.attr = attr;
        game->turn = 'O';
        game->state = GAME_READY;
        mutex_init(&game->lock);
        INIT_WORK(&game->ai_one_work, ai_one_work_func);
        INIT_WORK(&game->ai_two_work, ai_two_work_func);
        INIT_WORK(&game->drawboard_work, drawboard_work_func);
    }
    memset(ai_avgs, 0, sizeof(ai_avgs));

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    kxo_stop_work = false;
    rwlock_init(&attr_obj.lock);
    /* Setup the timers */
    timer_setup(&timer, timer_handler, 0);
    timer_setup(&loadavg_timer, loadavg_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo device: %d,%d\n", major, 0);
out:
    return ret;
error_workqueue:
    vfree(fast_buf.buf);
error_vmalloc:
    device_destroy(kxo_class, dev_id);
error_device:
    class_destroy(kxo_class);
error_cdev:
    cdev_del(&kxo_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_KMLDRV);
error_alloc:
    kfifo_free(&rx_fifo);
    goto out;
}

static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    kxo_set_work_stop(true);
    del_timer_sync(&timer);
    del_timer_sync(&loadavg_timer);
    tasklet_kill(&game_tasklet);
    flush_workqueue(kxo_workqueue);
    destroy_workqueue(kxo_workqueue);
    vfree(fast_buf.buf);
    device_destroy(kxo_class, dev_id);
    class_destroy(kxo_class);
    cdev_del(&kxo_cdev);
    unregister_chrdev_region(dev_id, NR_KMLDRV);
    free_rl_agent();
    free_negamax();
    free_mcts();
    zobrist_destroy();
    free_ai_sched();

    kfifo_free(&rx_fifo);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);
