/*
 * WRR Scheduling Class (mapped to the SCHED_WRR
 * policy)
 *
 * ZhouRong 516021910576
 */

#include "sched.h"
#include <linux/slab.h>

//debug.c
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/utsname.h>

static char group_path[PATH_MAX];

static char *task_group_path(struct task_group *tg)
{
	if (autogroup_path(tg, group_path, PATH_MAX)){
		printk(KERN_INFO "task_group_path part 1");
		return group_path;
	}

	/*
	 * May be NULL if the underlying cgroup isn't fully-created yet
	 */
	if (!tg->css.cgroup) {
		group_path[0] = '\0';
		printk(KERN_INFO "task_group_path part 2");
		return group_path;
	}
	cgroup_path(tg->css.cgroup, group_path, PATH_MAX);
	printk(KERN_INFO "task_group_path part 3");
	return group_path;
}


static int do_sched_wrr_period_timer(struct wrr_bandwidth *wrr_b, int overrun);

struct wrr_bandwidth def_wrr_bandwidth;

static enum hrtimer_restart sched_wrr_period_timer(struct hrtimer *timer)
{
	struct wrr_bandwidth *wrr_b =
		container_of(timer, struct wrr_bandwidth, wrr_period_timer);
	ktime_t now;
	int overrun;
	int idle = 0;

	for (;;) {
		now = hrtimer_cb_get_time(timer);
		overrun = hrtimer_forward(timer, now, wrr_b->wrr_period);

		if (!overrun)
			break;

		idle = do_sched_wrr_period_timer(wrr_b, overrun);
	}

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void init_wrr_bandwidth(struct wrr_bandwidth *wrr_b, u64 period, u64 runtime)
{
	wrr_b->wrr_period = ns_to_ktime(period);
	wrr_b->wrr_runtime = runtime;

	raw_spin_lock_init(&wrr_b->wrr_runtime_lock);

	hrtimer_init(&wrr_b->wrr_period_timer,
			CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	wrr_b->wrr_period_timer.function = sched_wrr_period_timer;
}

static void start_wrr_bandwidth(struct wrr_bandwidth *wrr_b)
{
	if (!wrr_bandwidth_enabled() || wrr_b->wrr_runtime == RUNTIME_INF)
		return;

	if (hrtimer_active(&wrr_b->wrr_period_timer))
		return;

	raw_spin_lock(&wrr_b->wrr_runtime_lock);
	start_bandwidth_timer(&wrr_b->wrr_period_timer, wrr_b->wrr_period);
	raw_spin_unlock(&wrr_b->wrr_runtime_lock);
}

void init_wrr_rq(struct wrr_rq *wrr_rq, struct rq *rq)
{
	struct wrr_prio_array *array;
	int i;

	array = &wrr_rq->active;
	for (i = 0; i < MAX_WRR_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_WRR_PRIO, array->bitmap);

#if defined CONFIG_SMP
	wrr_rq->highest_prio.curr = MAX_WRR_PRIO;
	wrr_rq->highest_prio.next = MAX_WRR_PRIO;
	wrr_rq->wrr_nr_migratory = 0;
	wrr_rq->overloaded = 0;
	plist_head_init(&wrr_rq->pushable_tasks);
#endif

	wrr_rq->wrr_time = 0;
	wrr_rq->wrr_throttled = 0;
	wrr_rq->wrr_runtime = 0;
	raw_spin_lock_init(&wrr_rq->wrr_runtime_lock);
}

#ifdef CONFIG_WRR_GROUP_SCHED
static void destroy_wrr_bandwidth(struct wrr_bandwidth *wrr_b)
{
	hrtimer_cancel(&wrr_b->wrr_period_timer);
}

#define wrr_entity_is_task(wrr_se) (!(wrr_se)->my_q)

static inline struct task_struct *wrr_task_of(struct sched_wrr_entity *wrr_se)
{
#ifdef CONFIG_SCHED_DEBUG
	WARN_ON_ONCE(!wrr_entity_is_task(wrr_se));
#endif
	return container_of(wrr_se, struct task_struct, wrr);
}

static inline struct rq *rq_of_wrr_rq(struct wrr_rq *wrr_rq)
{
	return wrr_rq->rq;
}

static inline struct wrr_rq *wrr_rq_of_se(struct sched_wrr_entity *wrr_se)
{
	return wrr_se->wrr_rq;
}

void free_wrr_sched_group(struct task_group *tg)
{
	int i;

	if (tg->wrr_se)
		destroy_wrr_bandwidth(&tg->wrr_bandwidth);

	for_each_possible_cpu(i) {
		if (tg->wrr_rq)
			kfree(tg->wrr_rq[i]);
		if (tg->wrr_se)
			kfree(tg->wrr_se[i]);
	}

	kfree(tg->wrr_rq);
	kfree(tg->wrr_se);
}

void init_tg_wrr_entry(struct task_group *tg, struct wrr_rq *wrr_rq,
		struct sched_wrr_entity *wrr_se, int cpu,
		struct sched_wrr_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	wrr_rq->highest_prio.curr = MAX_WRR_PRIO;
	wrr_rq->wrr_nr_boosted = 0;
	wrr_rq->rq = rq;
	wrr_rq->tg = tg;

	tg->wrr_rq[cpu] = wrr_rq;
	tg->wrr_se[cpu] = wrr_se;

	if (!wrr_se)
		return;

	if (!parent)
		wrr_se->wrr_rq = &rq->wrr;
	else
		wrr_se->wrr_rq = parent->my_q;

	wrr_se->my_q = wrr_rq;
	wrr_se->parent = parent;
	INIT_LIST_HEAD(&wrr_se->run_list);
}

int alloc_wrr_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct wrr_rq *wrr_rq;
	struct sched_wrr_entity *wrr_se;
	int i;

	tg->wrr_rq = kzalloc(sizeof(wrr_rq) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->wrr_rq)
		goto err;
	tg->wrr_se = kzalloc(sizeof(wrr_se) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->wrr_se)
		goto err;

	init_wrr_bandwidth(&tg->wrr_bandwidth,
			ktime_to_ns(def_wrr_bandwidth.wrr_period), 0);

	for_each_possible_cpu(i) {
		wrr_rq = kzalloc_node(sizeof(struct wrr_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!wrr_rq)
			goto err;

		wrr_se = kzalloc_node(sizeof(struct sched_wrr_entity),
				     GFP_KERNEL, cpu_to_node(i));
		if (!wrr_se)
			goto err_free_rq;

		init_wrr_rq(wrr_rq, cpu_rq(i));
		wrr_rq->wrr_runtime = tg->wrr_bandwidth.wrr_runtime;
		init_tg_wrr_entry(tg, wrr_rq, wrr_se, i, parent->wrr_se[i]);
	}

	return 1;

err_free_rq:
	kfree(wrr_rq);
err:
	return 0;
}

#else /* CONFIG_WRR_GROUP_SCHED */

#define wrr_entity_is_task(wrr_se) (1)

static inline struct task_struct *wrr_task_of(struct sched_wrr_entity *wrr_se)
{
	return container_of(wrr_se, struct task_struct, wrr);
}

static inline struct rq *rq_of_wrr_rq(struct wrr_rq *wrr_rq)
{
	return container_of(wrr_rq, struct rq, wrr);
}

static inline struct wrr_rq *wrr_rq_of_se(struct sched_wrr_entity *wrr_se)
{
	struct task_struct *p = wrr_task_of(wrr_se);
	struct rq *rq = task_rq(p);

	return &rq->wrr;
}

void free_wrr_sched_group(struct task_group *tg) { }

int alloc_wrr_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_WRR_GROUP_SCHED */

#ifdef CONFIG_SMP

static inline int wrr_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

static inline void wrr_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 */
	wmb();
	atomic_inc(&rq->rd->rto_count);
}

static inline void wrr_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

static void update_wrr_migration(struct wrr_rq *wrr_rq)
{
	if (wrr_rq->wrr_nr_migratory && wrr_rq->wrr_nr_total > 1) {
		if (!wrr_rq->overloaded) {
			wrr_set_overload(rq_of_wrr_rq(wrr_rq));
			wrr_rq->overloaded = 1;
		}
	} else if (wrr_rq->overloaded) {
		wrr_clear_overload(rq_of_wrr_rq(wrr_rq));
		wrr_rq->overloaded = 0;
	}
}

static void inc_wrr_migration(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{       printk(KERN_INFO "enter inc_wrr_migration successfully!!");
	if (!wrr_entity_is_task(wrr_se))
		return;

	wrr_rq = &rq_of_wrr_rq(wrr_rq)->wrr;

	wrr_rq->wrr_nr_total++;
	if (wrr_se->nr_cpus_allowed > 1)
		wrr_rq->wrr_nr_migratory++;

	update_wrr_migration(wrr_rq);
}

static void dec_wrr_migration(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{
	if (!wrr_entity_is_task(wrr_se))
		return;

	wrr_rq = &rq_of_wrr_rq(wrr_rq)->wrr;

	wrr_rq->wrr_nr_total--;
	if (wrr_se->nr_cpus_allowed > 1)
		wrr_rq->wrr_nr_migratory--;

	update_wrr_migration(wrr_rq);
}

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->wrr.pushable_tasks);
}

static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->wrr.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->wrr.pushable_tasks);

	/* Update the highest prio pushable task */
	if (p->prio < rq->wrr.highest_prio.next)
		rq->wrr.highest_prio.next = p->prio;
}

static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->wrr.pushable_tasks);

	/* Update the new highest prio pushable task */
	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->wrr.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->wrr.highest_prio.next = p->prio;
	} else
		rq->wrr.highest_prio.next = MAX_WRR_PRIO;
}

#else

static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_wrr_migration(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{ printk(KERN_INFO "enter inc_wrr_migration else else else successfully!!");
}

static inline
void dec_wrr_migration(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{
}

#endif /* CONFIG_SMP */

static inline int on_wrr_rq(struct sched_wrr_entity *wrr_se)
{
	return !list_empty(&wrr_se->run_list);
}

#ifdef CONFIG_WRR_GROUP_SCHED

static inline u64 sched_wrr_runtime(struct wrr_rq *wrr_rq)
{
	if (!wrr_rq->tg)
		return RUNTIME_INF;

	return wrr_rq->wrr_runtime;
}

static inline u64 sched_wrr_period(struct wrr_rq *wrr_rq)
{
	return ktime_to_ns(wrr_rq->tg->wrr_bandwidth.wrr_period);
}

typedef struct task_group *wrr_rq_iter_t;

static inline struct task_group *next_task_group(struct task_group *tg)
{
	do {
		tg = list_entry_rcu(tg->list.next,
			typeof(struct task_group), list);
	} while (&tg->list != &task_groups && task_group_is_autogroup(tg));

	if (&tg->list == &task_groups)
		tg = NULL;

	return tg;
}

#define for_each_wrr_rq(wrr_rq, iter, rq)					\
	for (iter = container_of(&task_groups, typeof(*iter), list);	\
		(iter = next_task_group(iter)) &&			\
		(wrr_rq = iter->wrr_rq[cpu_of(rq)]);)

static inline void list_add_leaf_wrr_rq(struct wrr_rq *wrr_rq)
{
	list_add_rcu(&wrr_rq->leaf_wrr_rq_list,
			&rq_of_wrr_rq(wrr_rq)->leaf_wrr_rq_list);
}

static inline void list_del_leaf_wrr_rq(struct wrr_rq *wrr_rq)
{
	list_del_rcu(&wrr_rq->leaf_wrr_rq_list);
}

#define for_each_leaf_wrr_rq(wrr_rq, rq) \
	list_for_each_entry_rcu(wrr_rq, &rq->leaf_wrr_rq_list, leaf_wrr_rq_list)

#define for_each_sched_wrr_entity(wrr_se) \
	for (; wrr_se; wrr_se = wrr_se->parent)

static inline struct wrr_rq *group_wrr_rq(struct sched_wrr_entity *wrr_se)
{
	return wrr_se->my_q;
}

static void enqueue_wrr_entity(struct sched_wrr_entity *wrr_se, bool head);
static void dequeue_wrr_entity(struct sched_wrr_entity *wrr_se);

static void sched_wrr_rq_enqueue(struct wrr_rq *wrr_rq)
{
	struct task_struct *curr = rq_of_wrr_rq(wrr_rq)->curr;
	struct sched_wrr_entity *wrr_se;

	int cpu = cpu_of(rq_of_wrr_rq(wrr_rq));

	wrr_se = wrr_rq->tg->wrr_se[cpu];

	if (wrr_rq->wrr_nr_running) {
		if (wrr_se && !on_wrr_rq(wrr_se))
			enqueue_wrr_entity(wrr_se, false);
		if (wrr_rq->highest_prio.curr < curr->prio)
			resched_task(curr);
	}
}

static void sched_wrr_rq_dequeue(struct wrr_rq *wrr_rq)
{
	struct sched_wrr_entity *wrr_se;
	int cpu = cpu_of(rq_of_wrr_rq(wrr_rq));

	wrr_se = wrr_rq->tg->wrr_se[cpu];

	if (wrr_se && on_wrr_rq(wrr_se))
		dequeue_wrr_entity(wrr_se);
}

static inline int wrr_rq_throttled(struct wrr_rq *wrr_rq)
{
	return wrr_rq->wrr_throttled && !wrr_rq->wrr_nr_boosted;
}

static int wrr_se_boosted(struct sched_wrr_entity *wrr_se)
{
	struct wrr_rq *wrr_rq = group_wrr_rq(wrr_se);
	struct task_struct *p;

	if (wrr_rq)
		return !!wrr_rq->wrr_nr_boosted;

	p = wrr_task_of(wrr_se);
	return p->prio != p->normal_prio;
}

#ifdef CONFIG_SMP
static inline const struct cpumask *sched_wrr_period_mask(void)
{
	return cpu_rq(smp_processor_id())->rd->span;
}
#else
static inline const struct cpumask *sched_wrr_period_mask(void)
{
	return cpu_online_mask;
}
#endif

static inline
struct wrr_rq *sched_wrr_period_wrr_rq(struct wrr_bandwidth *wrr_b, int cpu)
{
	return container_of(wrr_b, struct task_group, wrr_bandwidth)->wrr_rq[cpu];
}

static inline struct wrr_bandwidth *sched_wrr_bandwidth(struct wrr_rq *wrr_rq)
{
	return &wrr_rq->tg->wrr_bandwidth;
}

#else /* !CONFIG_WRR_GROUP_SCHED */

static inline u64 sched_wrr_runtime(struct wrr_rq *wrr_rq)
{
	return wrr_rq->wrr_runtime;
}

static inline u64 sched_wrr_period(struct wrr_rq *wrr_rq)
{
	return ktime_to_ns(def_wrr_bandwidth.wrr_period);
}

typedef struct wrr_rq *wrr_rq_iter_t;

#define for_each_wrr_rq(wrr_rq, iter, rq) \
	for ((void) iter, wrr_rq = &rq->wrr; wrr_rq; wrr_rq = NULL)

static inline void list_add_leaf_wrr_rq(struct wrr_rq *wrr_rq)
{
}

static inline void list_del_leaf_wrr_rq(struct wrr_rq *wrr_rq)
{
}

#define for_each_leaf_wrr_rq(wrr_rq, rq) \
	for (wrr_rq = &rq->wrr; wrr_rq; wrr_rq = NULL)

#define for_each_sched_wrr_entity(wrr_se) \
	for (; wrr_se; wrr_se = NULL)

static inline struct wrr_rq *group_wrr_rq(struct sched_wrr_entity *wrr_se)
{
	return NULL;
}

static inline void sched_wrr_rq_enqueue(struct wrr_rq *wrr_rq)
{
	if (wrr_rq->wrr_nr_running)
		resched_task(rq_of_wrr_rq(wrr_rq)->curr);
}

static inline void sched_wrr_rq_dequeue(struct wrr_rq *wrr_rq)
{
}

static inline int wrr_rq_throttled(struct wrr_rq *wrr_rq)
{
	return wrr_rq->wrr_throttled;
}

static inline const struct cpumask *sched_wrr_period_mask(void)
{
	return cpu_online_mask;
}

static inline
struct wrr_rq *sched_wrr_period_wrr_rq(struct wrr_bandwidth *wrr_b, int cpu)
{
	return &cpu_rq(cpu)->wrr;
}

static inline struct wrr_bandwidth *sched_wrr_bandwidth(struct wrr_rq *wrr_rq)
{
	return &def_wrr_bandwidth;
}

#endif /* CONFIG_WRR_GROUP_SCHED */

#ifdef CONFIG_SMP
/*
 * We ran out of runtime, see if we can borrow some from our neighbours.
 */
static int do_balance_runtime(struct wrr_rq *wrr_rq)
{
	struct wrr_bandwidth *wrr_b = sched_wrr_bandwidth(wrr_rq);
	struct root_domain *rd = rq_of_wrr_rq(wrr_rq)->rd;
	int i, weight, more = 0;
	u64 wrr_period;

	weight = cpumask_weight(rd->span);

	raw_spin_lock(&wrr_b->wrr_runtime_lock);
	wrr_period = ktime_to_ns(wrr_b->wrr_period);
	for_each_cpu(i, rd->span) {
		struct wrr_rq *iter = sched_wrr_period_wrr_rq(wrr_b, i);
		s64 diff;

		if (iter == wrr_rq)
			continue;

		raw_spin_lock(&iter->wrr_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disalow stealing.
		 */
		if (iter->wrr_runtime == RUNTIME_INF)
			goto next;

		/*
		 * From runqueues with spare time, take 1/n part of their
		 * spare time, but no more than our period.
		 */
		diff = iter->wrr_runtime - iter->wrr_time;
		if (diff > 0) {
			diff = div_u64((u64)diff, weight);
			if (wrr_rq->wrr_runtime + diff > wrr_period)
				diff = wrr_period - wrr_rq->wrr_runtime;
			iter->wrr_runtime -= diff;
			wrr_rq->wrr_runtime += diff;
			more = 1;
			if (wrr_rq->wrr_runtime == wrr_period) {
				raw_spin_unlock(&iter->wrr_runtime_lock);
				break;
			}
		}
next:
		raw_spin_unlock(&iter->wrr_runtime_lock);
	}
	raw_spin_unlock(&wrr_b->wrr_runtime_lock);

	return more;
}

/*
 * Ensure this RQ takes back all the runtime it lend to its neighbours.
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	wrr_rq_iter_t iter;
	struct wrr_rq *wrr_rq;

	if (unlikely(!scheduler_running))
		return;

	for_each_wrr_rq(wrr_rq, iter, rq) {
		struct wrr_bandwidth *wrr_b = sched_wrr_bandwidth(wrr_rq);
		s64 want;
		int i;

		raw_spin_lock(&wrr_b->wrr_runtime_lock);
		raw_spin_lock(&wrr_rq->wrr_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		if (wrr_rq->wrr_runtime == RUNTIME_INF ||
				wrr_rq->wrr_runtime == wrr_b->wrr_runtime)
			goto balanced;
		raw_spin_unlock(&wrr_rq->wrr_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		want = wrr_b->wrr_runtime - wrr_rq->wrr_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		for_each_cpu(i, rd->span) {
			struct wrr_rq *iter = sched_wrr_period_wrr_rq(wrr_b, i);
			s64 diff;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == wrr_rq || iter->wrr_runtime == RUNTIME_INF)
				continue;

			raw_spin_lock(&iter->wrr_runtime_lock);
			if (want > 0) {
				diff = min_t(s64, iter->wrr_runtime, want);
				iter->wrr_runtime -= diff;
				want -= diff;
			} else {
				iter->wrr_runtime -= want;
				want -= want;
			}
			raw_spin_unlock(&iter->wrr_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&wrr_rq->wrr_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		BUG_ON(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		wrr_rq->wrr_runtime = RUNTIME_INF;
		raw_spin_unlock(&wrr_rq->wrr_runtime_lock);
		raw_spin_unlock(&wrr_b->wrr_runtime_lock);
	}
}

static void disable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__disable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static void __enable_runtime(struct rq *rq)
{
	wrr_rq_iter_t iter;
	struct wrr_rq *wrr_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	for_each_wrr_rq(wrr_rq, iter, rq) {
		struct wrr_bandwidth *wrr_b = sched_wrr_bandwidth(wrr_rq);

		raw_spin_lock(&wrr_b->wrr_runtime_lock);
		raw_spin_lock(&wrr_rq->wrr_runtime_lock);
		wrr_rq->wrr_runtime = wrr_b->wrr_runtime;
		wrr_rq->wrr_time = 0;
		wrr_rq->wrr_throttled = 0;
		raw_spin_unlock(&wrr_rq->wrr_runtime_lock);
		raw_spin_unlock(&wrr_b->wrr_runtime_lock);
	}
}

static void enable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__enable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

int update_runtime(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	int cpu = (int)(long)hcpu;

	switch (action) {
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		disable_runtime(cpu_rq(cpu));
		return NOTIFY_OK;

	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		enable_runtime(cpu_rq(cpu));
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

static int balance_runtime(struct wrr_rq *wrr_rq)
{
	int more = 0;

	if (!sched_feat(WRR_RUNTIME_SHARE))
		return more;

	if (wrr_rq->wrr_time > wrr_rq->wrr_runtime) {
		raw_spin_unlock(&wrr_rq->wrr_runtime_lock);
		more = do_balance_runtime(wrr_rq);
		raw_spin_lock(&wrr_rq->wrr_runtime_lock);
	}

	return more;
}
#else /* !CONFIG_SMP */
static inline int balance_runtime(struct wrr_rq *wrr_rq)
{
	return 0;
}
#endif /* CONFIG_SMP */

static int do_sched_wrr_period_timer(struct wrr_bandwidth *wrr_b, int overrun)
{
	int i, idle = 1, throttled = 0;
	const struct cpumask *span;

	span = sched_wrr_period_mask();
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct wrr_rq *wrr_rq = sched_wrr_period_wrr_rq(wrr_b, i);
		struct rq *rq = rq_of_wrr_rq(wrr_rq);

		raw_spin_lock(&rq->lock);
		if (wrr_rq->wrr_time) {
			u64 runtime;

			raw_spin_lock(&wrr_rq->wrr_runtime_lock);
			if (wrr_rq->wrr_throttled)
				balance_runtime(wrr_rq);
			runtime = wrr_rq->wrr_runtime;
			wrr_rq->wrr_time -= min(wrr_rq->wrr_time, overrun*runtime);
			if (wrr_rq->wrr_throttled && wrr_rq->wrr_time < runtime) {
				wrr_rq->wrr_throttled = 0;
				enqueue = 1;

				/*
				 * Force a clock update if the CPU was idle,
				 * lest wakeup -> unthrottle time accumulate.
				 */
				if (wrr_rq->wrr_nr_running && rq->curr == rq->idle)
					rq->skip_clock_update = -1;
			}
			if (wrr_rq->wrr_time || wrr_rq->wrr_nr_running)
				idle = 0;
			raw_spin_unlock(&wrr_rq->wrr_runtime_lock);
		} else if (wrr_rq->wrr_nr_running) {
			idle = 0;
			if (!wrr_rq_throttled(wrr_rq))
				enqueue = 1;
		}
		if (wrr_rq->wrr_throttled)
			throttled = 1;

		if (enqueue)
			sched_wrr_rq_enqueue(wrr_rq);
		raw_spin_unlock(&rq->lock);
	}

	if (!throttled && (!wrr_bandwidth_enabled() || wrr_b->wrr_runtime == RUNTIME_INF))
		return 1;

	return idle;
}

static inline int wrr_se_prio(struct sched_wrr_entity *wrr_se)
{{ printk(KERN_INFO "enter wrr_se_prio successfully!!");}
#ifdef CONFIG_WRR_GROUP_SCHED
	struct wrr_rq *wrr_rq = group_wrr_rq(wrr_se);

	if (wrr_rq)
		return wrr_rq->highest_prio.curr;
#endif

	return wrr_task_of(wrr_se)->prio;
}

static int sched_wrr_runtime_exceeded(struct wrr_rq *wrr_rq)
{
	u64 runtime = sched_wrr_runtime(wrr_rq);

	if (wrr_rq->wrr_throttled)
		return wrr_rq_throttled(wrr_rq);

	if (runtime >= sched_wrr_period(wrr_rq))
		return 0;

	balance_runtime(wrr_rq);
	runtime = sched_wrr_runtime(wrr_rq);
	if (runtime == RUNTIME_INF)
		return 0;

	if (wrr_rq->wrr_time > runtime) {
		struct wrr_bandwidth *wrr_b = sched_wrr_bandwidth(wrr_rq);

		/*
		 * Don't actually throttle groups that have no runtime assigned
		 * but accrue some time due to boosting.
		 */
		if (likely(wrr_b->wrr_runtime)) {
			static bool once = false;

			wrr_rq->wrr_throttled = 1;

			if (!once) {
				once = true;
				printk_sched("sched: wrr throttling activated\n");
			}
		} else {
			/*
			 * In case we did anyway, make it go away,
			 * replenishment is a joke, since it will replenish us
			 * with exactly 0 ns.
			 */
			wrr_rq->wrr_time = 0;
		}

		if (wrr_rq_throttled(wrr_rq)) {
			sched_wrr_rq_dequeue(wrr_rq);
			return 1;
		}
	}

	return 0;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_wrr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_wrr_entity *wrr_se = &curr->wrr;
	struct wrr_rq *wrr_rq = wrr_rq_of_se(wrr_se);
	u64 delta_exec;

	if (curr->sched_class != &wrr_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq->clock_task;
	cpuacct_charge(curr, delta_exec);

	sched_wrr_avg_update(rq, delta_exec);

	if (!wrr_bandwidth_enabled())
		return;

	for_each_sched_wrr_entity(wrr_se) {
		wrr_rq = wrr_rq_of_se(wrr_se);

		if (sched_wrr_runtime(wrr_rq) != RUNTIME_INF) {
			raw_spin_lock(&wrr_rq->wrr_runtime_lock);
			wrr_rq->wrr_time += delta_exec;
			if (sched_wrr_runtime_exceeded(wrr_rq))
				resched_task(curr);
			raw_spin_unlock(&wrr_rq->wrr_runtime_lock);
		}
	}
}

#if defined CONFIG_SMP

static void
inc_wrr_prio_smp(struct wrr_rq *wrr_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_wrr_rq(wrr_rq);

	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

static void
dec_wrr_prio_smp(struct wrr_rq *wrr_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_wrr_rq(wrr_rq);

	if (rq->online && wrr_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, wrr_rq->highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_wrr_prio_smp(struct wrr_rq *wrr_rq, int prio, int prev_prio) {}
static inline
void dec_wrr_prio_smp(struct wrr_rq *wrr_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined CONFIG_SMP || defined CONFIG_WRR_GROUP_SCHED
static void
inc_wrr_prio(struct wrr_rq *wrr_rq, int prio)
{      printk(KERN_INFO "enter inc_wrr_prio successfully!!");
	int prev_prio = wrr_rq->highest_prio.curr;

	if (prio < prev_prio)
		wrr_rq->highest_prio.curr = prio;

	inc_wrr_prio_smp(wrr_rq, prio, prev_prio);
}

static void
dec_wrr_prio(struct wrr_rq *wrr_rq, int prio)
{
	int prev_prio = wrr_rq->highest_prio.curr;

	if (wrr_rq->wrr_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct wrr_prio_array *array = &wrr_rq->active;

			wrr_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else
		wrr_rq->highest_prio.curr = MAX_wrr_PRIO;

	dec_wrr_prio_smp(wrr_rq, prio, prev_prio);
}

#else

static inline void inc_wrr_prio(struct wrr_rq *wrr_rq, int prio) { printk(KERN_INFO "enter inc_wrr_prio else else else successfully!!");}
static inline void dec_wrr_prio(struct wrr_rq *wrr_rq, int prio) {}

#endif /* CONFIG_SMP || CONFIG_WRR_GROUP_SCHED */

#ifdef CONFIG_WRR_GROUP_SCHED

static void
inc_wrr_group(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{  printk(KERN_INFO "enter inc_wrr_group successfully!!");
	if (wrr_se_boosted(wrr_se))
		wrr_rq->wrr_nr_boosted++;

	if (wrr_rq->tg)
		start_wrr_bandwidth(&wrr_rq->tg->wrr_bandwidth);
}

static void
dec_wrr_group(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{
	if (wrr_se_boosted(wrr_se))
		wrr_rq->wrr_nr_boosted--;

	WARN_ON(!wrr_rq->wrr_nr_running && wrr_rq->wrr_nr_boosted);
}

#else /* CONFIG_WRR_GROUP_SCHED */

static void
inc_wrr_group(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{
	start_wrr_bandwidth(&def_wrr_bandwidth);
}

static inline
void dec_wrr_group(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq) {}

#endif /* CONFIG_WRR_GROUP_SCHED */

static inline
void inc_wrr_tasks(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{printk(KERN_INFO "inc_wrr_tasks step 1");
	int prio = wrr_se_prio(wrr_se);
printk(KERN_INFO "inc_wrr_tasks step 2");
//	WARN_ON(!wrr_prio(prio));
printk(KERN_INFO "inc_wrr_tasks step 3");
	wrr_rq->wrr_nr_running++;
printk(KERN_INFO "inc_wrr_tasks step 4");
	inc_wrr_prio(wrr_rq, prio);
printk(KERN_INFO "inc_wrr_tasks step 5");
	//inc_wrr_migration(wrr_se, wrr_rq);
printk(KERN_INFO "inc_wrr_tasks step 6");
	//inc_wrr_group(wrr_se, wrr_rq);
        printk(KERN_INFO "@@@inc_wrr_tasks success");
}

static inline
void dec_wrr_tasks(struct sched_wrr_entity *wrr_se, struct wrr_rq *wrr_rq)
{
	WARN_ON(!wrr_prio(wrr_se_prio(wrr_se)));
	WARN_ON(!wrr_rq->wrr_nr_running);
	wrr_rq->wrr_nr_running--;

	dec_wrr_prio(wrr_rq, wrr_se_prio(wrr_se));
	dec_wrr_migration(wrr_se, wrr_rq);
	dec_wrr_group(wrr_se, wrr_rq);
}

static void __enqueue_wrr_entity(struct sched_wrr_entity *wrr_se, bool head)
{
	struct wrr_rq *wrr_rq = wrr_rq_of_se(wrr_se);
	struct wrr_prio_array *array = &wrr_rq->active;
	struct wrr_rq *group_rq = group_wrr_rq(wrr_se);
	struct list_head *queue = array->queue + wrr_se_prio(wrr_se);

	/*
	 * Don't enqueue the group if its throttled, or when empty.
	 * The latter is a consequence of the former when a child group
	 * get throttled and the current group doesn't have any other
	 * active members.
	 */
printk(KERN_INFO "__enqueue_wrr_entity part 1");
	if (group_rq && (wrr_rq_throttled(group_rq) || !group_rq->wrr_nr_running))
		return;
printk(KERN_INFO "__enqueue_wrr_entity part 2");
	if (!wrr_rq->wrr_nr_running)
		list_add_leaf_wrr_rq(wrr_rq);
printk(KERN_INFO "__enqueue_wrr_entity part 3");
	/**
	if (head)
		list_add(&wrr_se->run_list, queue);
	else
		list_add_tail(&wrr_se->run_list, queue);
	**/
printk(KERN_INFO "__enqueue_wrr_entity part 3.5");
	__set_bit(wrr_se_prio(wrr_se), array->bitmap);
printk(KERN_INFO "__enqueue_wrr_entity part 4");
	inc_wrr_tasks(wrr_se, wrr_rq);
printk(KERN_INFO "__enqueue_wrr_entity part 5");
}

static void __dequeue_wrr_entity(struct sched_wrr_entity *wrr_se)
{
	struct wrr_rq *wrr_rq = wrr_rq_of_se(wrr_se);
	struct wrr_prio_array *array = &wrr_rq->active;

	list_del_init(&wrr_se->run_list);
	if (list_empty(array->queue + wrr_se_prio(wrr_se)))
		__clear_bit(wrr_se_prio(wrr_se), array->bitmap);

	dec_wrr_tasks(wrr_se, wrr_rq);
	if (!wrr_rq->wrr_nr_running)
		list_del_leaf_wrr_rq(wrr_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
static void dequeue_wrr_stack(struct sched_wrr_entity *wrr_se)
{
	struct sched_wrr_entity *back = NULL;

	for_each_sched_wrr_entity(wrr_se) {
		wrr_se->back = back;
		back = wrr_se;
	}

	for (wrr_se = back; wrr_se; wrr_se = wrr_se->back) {
		if (on_wrr_rq(wrr_se))
			__dequeue_wrr_entity(wrr_se);
	}
}

static void enqueue_wrr_entity(struct sched_wrr_entity *wrr_se, bool head)
{
	dequeue_wrr_stack(wrr_se);
	for_each_sched_wrr_entity(wrr_se)
		__enqueue_wrr_entity(wrr_se, head);
}

static void dequeue_wrr_entity(struct sched_wrr_entity *wrr_se)
{
	dequeue_wrr_stack(wrr_se);

	for_each_sched_wrr_entity(wrr_se) {
		struct wrr_rq *wrr_rq = group_wrr_rq(wrr_se);

		if (wrr_rq && wrr_rq->wrr_nr_running)
			__enqueue_wrr_entity(wrr_se, false);
	}
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
        printk("enqueue_task_wrr enqueue_task_wrr enqueue_task_wrr part 1 part 1 part 1 part 1 part 1");
	if (flags & ENQUEUE_WAKEUP)
		wrr_se->timeout = 0;
        printk("enqueue_task_wrr enqueue_task_wrr enqueue_task_wrr part 2 part 2 part 2 part 2 part 2");
	enqueue_wrr_entity(wrr_se, flags & ENQUEUE_HEAD);
        printk("enqueue_task_wrr enqueue_task_wrr enqueue_task_wrr part 3 part 3 part 3 part 3 part 3");
	if (!task_current(rq, p) && p->wrr.nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
        printk("enqueue_task_wrr enqueue_task_wrr enqueue_task_wrr part 4 part 4 part 4 part 4 part 4");
	inc_nr_running(rq);
}

static void dequeue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;

	update_curr_wrr(rq);
	dequeue_wrr_entity(wrr_se);

	dequeue_pushable_task(rq, p);

	dec_nr_running(rq);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_wrr_entity(struct wrr_rq *wrr_rq, struct sched_wrr_entity *wrr_se, int head)
{
	if (on_wrr_rq(wrr_se)) {
		struct wrr_prio_array *array = &wrr_rq->active;
		struct list_head *queue = array->queue + wrr_se_prio(wrr_se);

		if (head)
			list_move(&wrr_se->run_list, queue);
		else
			list_move_tail(&wrr_se->run_list, queue);
	}
}

static void requeue_task_wrr(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	struct wrr_rq *wrr_rq;

	for_each_sched_wrr_entity(wrr_se) {
		wrr_rq = wrr_rq_of_se(wrr_se);
		requeue_wrr_entity(wrr_rq, wrr_se, head);
	}
}

static void yield_task_wrr(struct rq *rq)
{
	requeue_task_wrr(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

static int
select_task_rq_wrr(struct task_struct *p, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;
	int cpu;

	cpu = task_cpu(p);

	if (p->wrr.nr_cpus_allowed == 1)
		goto out;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = ACCESS_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an WRR task, then
	 * try to see if we can wake this WRR task up on another
	 * runqueue. Otherwise simply start this WRR task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	if (curr && unlikely(wrr_task(curr)) &&
	    (curr->wrr.nr_cpus_allowed < 2 ||
	     curr->prio <= p->prio) &&
	    (p->wrr.nr_cpus_allowed > 1)) {
		int target = find_lowest_rq(p);

		if (target != -1)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	if (rq->curr->wrr.nr_cpus_allowed == 1)
		return;

	if (p->wrr.nr_cpus_allowed != 1
	    && cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	if (!cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * There appears to be other cpus that can accept
	 * current and none to run 'p', so lets reschedule
	 * to try and push current away:
	 */
	requeue_task_wrr(rq, p, 1);
	resched_task(rq->curr);
}

#endif /* CONFIG_SMP */

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_task(rq->curr);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

static struct sched_wrr_entity *pick_next_wrr_entity(struct rq *rq,
						   struct wrr_rq *wrr_rq)
{
	struct wrr_prio_array *array = &wrr_rq->active;
	struct sched_wrr_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_WRR_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_wrr_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_wrr(struct rq *rq)
{
	struct sched_wrr_entity *wrr_se;
	struct task_struct *p;
	struct wrr_rq *wrr_rq;

	wrr_rq = &rq->wrr;

	if (!wrr_rq->wrr_nr_running)
		return NULL;

	if (wrr_rq_throttled(wrr_rq))
		return NULL;

	do {
		wrr_se = pick_next_wrr_entity(rq, wrr_rq);
		BUG_ON(!wrr_se);
		wrr_rq = group_wrr_rq(wrr_se);
	} while (wrr_rq);

	p = wrr_task_of(wrr_se);
	p->se.exec_start = rq->clock_task;

	return p;
}

static struct task_struct *pick_next_task_wrr(struct rq *rq)
{
	struct task_struct *p = _pick_next_task_wrr(rq);

	/* The running task is never eligible for pushing */
	if (p)
		dequeue_pushable_task(rq, p);

#ifdef CONFIG_SMP
	/*
	 * We detect this state here so that we can avoid taking the RQ
	 * lock again later if there is no need to push
	 */
	rq->post_schedule = has_pushable_tasks(rq);
#endif

	return p;
}

static void put_prev_task_wrr(struct rq *rq, struct task_struct *p)
{
	update_curr_wrr(rq);

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	if (on_wrr_rq(&p->wrr) && p->wrr.nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define WRR_MAX_TRIES 3

static int pick_wrr_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    (cpu < 0 || cpumask_test_cpu(cpu, tsk_cpus_allowed(p))) &&
	    (p->wrr.nr_cpus_allowed > 1))
		return 1;
	return 0;
}

/* Return the second highest WRR task, NULL otherwise */
static struct task_struct *pick_next_highest_task_wrr(struct rq *rq, int cpu)
{
	struct task_struct *next = NULL;
	struct sched_wrr_entity *wrr_se;
	struct wrr_prio_array *array;
	struct wrr_rq *wrr_rq;
	int idx;

	for_each_leaf_wrr_rq(wrr_rq, rq) {
		array = &wrr_rq->active;
		idx = sched_find_first_bit(array->bitmap);
next_idx:
		if (idx >= MAX_WRR_PRIO)
			continue;
		if (next && next->prio <= idx)
			continue;
		list_for_each_entry(wrr_se, array->queue + idx, run_list) {
			struct task_struct *p;

			if (!wrr_entity_is_task(wrr_se))
				continue;

			p = wrr_task_of(wrr_se);
			if (pick_wrr_task(rq, p, cpu)) {
				next = p;
				break;
			}
		}
		if (!next) {
			idx = find_next_bit(array->bitmap, MAX_wrr_PRIO, idx+1);
			goto next_idx;
		}
	}

	return next;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = __get_cpu_var(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->wrr.nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of cpus representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last cpu that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which cpu is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;
	return -1;
}

/* Will lock the rq it finds */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < WRR_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu,
						       tsk_cpus_allowed(task)) ||
				     task_running(rq, task) ||
				     !task->on_rq)) {

				raw_spin_unlock(&lowest_rq->lock);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		if (lowest_rq->wrr.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	p = plist_first_entry(&rq->wrr.pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->wrr.nr_cpus_allowed <= 1);

	BUG_ON(!p->on_rq);
	BUG_ON(!wrr_task(p));

	return p;
}

/*
 * If the current CPU has more than one WRR task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
static int push_wrr_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;

	if (!rq->wrr.overloaded)
		return 0;

	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

#ifdef __ARCH_WANT_INTERRUPTS_ON_CTXSW
       if (unlikely(task_running(rq, next_task)))
               return 0;
#endif

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_task(rq->curr);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		task = pick_next_pushable_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other cpus will pull from us when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);
	ret = 1;

	resched_task(lowest_rq->curr);

	double_unlock_balance(rq, lowest_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_wrr_tasks(struct rq *rq)
{
	/* push_wrr_task will return true if it moved an WRR */
	while (push_wrr_task(rq))
		;
}

static int pull_wrr_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;

	if (likely(!wrr_overloaded(this_rq)))
		return 0;

	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		if (src_rq->wrr.highest_prio.next >=
		    this_rq->wrr.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * Are there still pullable WRR tasks?
		 */
		if (src_rq->wrr.wrr_nr_running <= 1)
			goto skip;

		p = pick_next_highest_task_wrr(src_rq, this_cpu);

		/*
		 * Do we have an WRR task that preempts
		 * the to-be-scheduled task?
		 */
		if (p && (p->prio < this_rq->wrr.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->on_rq);

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its cpu.
			 * This is just that p is wakeing up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

			ret = 1;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	return ret;
}

static void pre_schedule_wrr(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull WRR tasks here if we lower this rq's prio */
	if (rq->wrr.highest_prio.curr > prev->prio)
		pull_wrr_task(rq);
}

static void post_schedule_wrr(struct rq *rq)
{
	push_wrr_tasks(rq);
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_wrr(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    has_pushable_tasks(rq) &&
	    p->wrr.nr_cpus_allowed > 1 &&
	    wrr_task(rq->curr) &&
	    (rq->curr->wrr.nr_cpus_allowed < 2 ||
	     rq->curr->prio <= p->prio))
		push_wrr_tasks(rq);
}

static void set_cpus_allowed_wrr(struct task_struct *p,
				const struct cpumask *new_mask)
{
	int weight = cpumask_weight(new_mask);

	BUG_ON(!wrr_task(p));

	/*
	 * Update the migration status of the RQ if we have an WRR task
	 * which is running AND changing its weight value.
	 */
	if (p->on_rq && (weight != p->wrr.nr_cpus_allowed)) {
		struct rq *rq = task_rq(p);

		if (!task_current(rq, p)) {
			/*
			 * Make sure we dequeue this task from the pushable list
			 * before going further.  It will either remain off of
			 * the list because we are no longer pushable, or it
			 * will be requeued.
			 */
			if (p->wrr.nr_cpus_allowed > 1)
				dequeue_pushable_task(rq, p);

			/*
			 * Requeue if our weight is changing and still > 1
			 */
			if (weight > 1)
				enqueue_pushable_task(rq, p);

		}

		if ((p->wrr.nr_cpus_allowed <= 1) && (weight > 1)) {
			rq->wrr.wrr_nr_migratory++;
		} else if ((p->wrr.nr_cpus_allowed > 1) && (weight <= 1)) {
			BUG_ON(!rq->wrr.wrr_nr_migratory);
			rq->wrr.wrr_nr_migratory--;
		}

		update_wrr_migration(&rq->wrr);
	}
}

/* Assumes rq->lock is held */
static void rq_online_wrr(struct rq *rq)
{
	if (rq->wrr.overloaded)
		wrr_set_overload(rq);

	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->wrr.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_wrr(struct rq *rq)
{
	if (rq->wrr.overloaded)
		wrr_clear_overload(rq);

	__disable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the WRR queue, we bring ourselves to a position
 * that we might want to pull WRR tasks from other runqueues.
 */
static void switched_from_wrr(struct rq *rq, struct task_struct *p)
{
	/*
	 * If there are other wrr tasks then we will reschedule
	 * and the scheduling of the other WRR tasks will handle
	 * the balancing. But if we are the last WRR task
	 * we may need to handle the pulling of WRR tasks
	 * now.
	 */
	if (p->on_rq && !rq->wrr.wrr_nr_running)
		pull_wrr_task(rq);
}

void init_sched_wrr_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}
}
#endif /* CONFIG_SMP */

/*
 * When switching a task to WRR, we may overload the runqueue
 * with WRR tasks. In this case we try to push them off to
 * other runqueues.
 */
static void switched_to_wrr(struct rq *rq, struct task_struct *p)
{
	int check_resched = 1;

	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an WRR task
	 * then see if we can move to another run queue.
	 */
	if (p->on_rq && rq->curr != p) {
#ifdef CONFIG_SMP
		if (rq->wrr.overloaded && push_wrr_task(rq) &&
		    /* Don't resched if we changed runqueues */
		    rq != task_rq(p))
			check_resched = 0;
#endif /* CONFIG_SMP */
		if (check_resched && p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_wrr(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->on_rq)
		return;

	if (rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			pull_wrr_task(rq);
		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule. Note, the above pull_wrr_task
		 * can release the rq lock and p could migrate.
		 * Only reschedule if p is still on the same runqueue.
		 */
		if (p->prio > rq->wrr.highest_prio.curr && rq->curr == p)
			resched_task(p);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_task(p);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		p->wrr.timeout++;
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->wrr.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}

//static char path[PATH_MAX];
static void task_tick_wrr(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;

	update_curr_wrr(rq);

	watchdog(rq, p);


	if (--p->wrr.time_slice)
		return;

    //strcpy(path,task_group_path(task_group(p)));
	if(strcmp(task_group_path(task_group(p)),"/")==0){
       printk(KERN_INFO "IN task_tick_wrr, the fg_interval is %d ms\n",WRRF_TIMESLICE);
	   p->wrr.time_slice = WRRF_TIMESLICE;
    } else if (strcmp(task_group_path(task_group(p)),"/bg_non_interactive")==0){
       printk(KERN_INFO "IN task_tick_wrr, the bg_interval is %d ms\n",WRRB_TIMESLICE);
       p->wrr.time_slice = WRRB_TIMESLICE;
    }

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are the
	 * only element on the queue
	 */
	for_each_sched_wrr_entity(wrr_se) {
		if (wrr_se->run_list.prev != wrr_se->run_list.next) {
			requeue_task_wrr(rq, p, 0);
			set_tsk_need_resched(p);
			return;
		}
	}
}

static void set_curr_task_wrr(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock_task;

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rq, p);
}

//static char app_path[PATH_MAX];
static unsigned int get_rr_interval_wrr(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 10ms for Background, 100ms for Foreground.
	 */
    //strcpy(app_path,task_group_path(task_group(task)));
    printk(KERN_INFO "IN get_rr_interval_wrr, the app_path is %s, strlen %d\n",task_group_path(task_group(task)),strlen(task_group_path(task_group(task))));

	if (strcmp(task_group_path(task_group(task)),"/")==0){
        printk(KERN_INFO "IN get_rr_interval_wrr, the fg_interval is %d ms\n",WRRF_TIMESLICE*10);
        return WRRF_TIMESLICE;
    }else if (strcmp(task_group_path(task_group(task)),"/bg_non_interactive")==0){
        printk(KERN_INFO "IN get_rr_interval_wrr, the bg_interval is %d ms\n",WRRB_TIMESLICE*10);
        return WRRB_TIMESLICE;
    }
}

const struct sched_class wrr_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_wrr,
	.dequeue_task		= dequeue_task_wrr,
	.yield_task		= yield_task_wrr,

	.check_preempt_curr	= check_preempt_curr_wrr,

	.pick_next_task		= pick_next_task_wrr,
	.put_prev_task		= put_prev_task_wrr,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_wrr,

	.set_cpus_allowed       = set_cpus_allowed_wrr,
	.rq_online              = rq_online_wrr,
	.rq_offline             = rq_offline_wrr,
	.pre_schedule		= pre_schedule_wrr,
	.post_schedule		= post_schedule_wrr,
	.task_woken		= task_woken_wrr,
	.switched_from		= switched_from_wrr,
#endif

	.set_curr_task          = set_curr_task_wrr,
	.task_tick		= task_tick_wrr,

	.get_rr_interval	= get_rr_interval_wrr,

	.prio_changed		= prio_changed_wrr,
	.switched_to		= switched_to_wrr,
};
