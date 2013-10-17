/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2006
 * Copyright (C) Fujitsu, 2012-2013
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *	   Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		Documentation/RCU/ *.txt
 *
 */

#include <linux/export.h>
#include <linux/sched.h>
#include <linux/srcu.h>

#include <trace/events/rcu.h>

#include "rcu.h"

/*
 * Initialize an rcu_batch structure to empty.
 */
static inline void rcu_batch_init(struct rcu_batch *b)
{
	b->head = NULL;
	b->tail = &b->head;
}

/*
 * Enqueue a callback onto the tail of the specified rcu_batch structure.
 */
static inline void rcu_batch_queue(struct rcu_batch *b, struct rcu_head *head)
{
	*b->tail = head;
	b->tail = &head->next;
}

/*
 * Is the specified rcu_batch structure empty?
 */
static inline bool rcu_batch_empty(struct rcu_batch *b)
{
	return b->tail == &b->head;
}

/*
 * Tests whether the specified rcu_batch structure has just one entry.
 */
static inline bool rcu_batch_is_singular(struct rcu_batch *b)
{
	return container_of(b->tail, struct rcu_head, next) == b->head;
}

/*
 * Remove the callback at the head of the specified rcu_batch structure
 * and return a pointer to it, or return NULL if the structure is empty.
 */
static inline struct rcu_head *rcu_batch_dequeue(struct rcu_batch *b)
{
	struct rcu_head *head;

	if (rcu_batch_empty(b))
		return NULL;

	head = b->head;
	b->head = head->next;
	if (b->tail == &head->next)
		rcu_batch_init(b);

	return head;
}

/*
 * Move all callbacks from the rcu_batch structure specified by "from" to
 * the structure specified by "to".
 */
static inline void rcu_batch_move(struct rcu_batch *to, struct rcu_batch *from)
{
	if (!rcu_batch_empty(from)) {
		*to->tail = from->head;
		to->tail = from->tail;
		rcu_batch_init(from);
	}
}

static int init_srcu_struct_fields(struct srcu_struct *sp)
{
	sp->completed = 0;
	atomic_set(&sp->gp_ref, 0);
	spin_lock_init(&sp->lock);
	rcu_batch_init(&sp->batch_queue);
	rcu_batch_init(&sp->batch_wait_gp);
	rcu_batch_init(&sp->batch_done);
	INIT_WORK(&sp->work, process_srcu);
	sp->per_cpu_ref = alloc_percpu(struct srcu_struct_array);
	return sp->per_cpu_ref ? 0 : -ENOMEM;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_srcu_struct(struct srcu_struct *sp, const char *name,
		       struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	debug_check_no_locks_freed((void *)sp, sizeof(*sp));
	lockdep_init_map(&sp->dep_map, name, key, 0);
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * init_srcu_struct - initialize a sleep-RCU structure
 * @sp: structure to initialize.
 *
 * Must invoke this on a given srcu_struct before passing that srcu_struct
 * to any other function.  Each srcu_struct represents a separate domain
 * of SRCU protection.
 */
int init_srcu_struct(struct srcu_struct *sp)
{
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(init_srcu_struct);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * srcu_readers_active - returns approximate number of readers.
 * @sp: which srcu_struct to count active readers (holding srcu_read_lock).
 *
 * Note that this is not an atomic primitive, and can therefore suffer
 * severe errors when invoked on an active srcu_struct.  That said, it
 * can be useful as an error check at cleanup time.
 */
static int srcu_readers_active(struct srcu_struct *sp)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		sum += ACCESS_ONCE(per_cpu_ptr(sp->per_cpu_ref, cpu)->c[0]);
		sum += ACCESS_ONCE(per_cpu_ptr(sp->per_cpu_ref, cpu)->c[1]);
	}
	return sum;
}

/**
 * cleanup_srcu_struct - deconstruct a sleep-RCU structure
 * @sp: structure to clean up.
 *
 * Must invoke this after you are finished using a given srcu_struct that
 * was initialized via init_srcu_struct(), else you leak memory.
 */
void cleanup_srcu_struct(struct srcu_struct *sp)
{
	if (WARN_ON(srcu_readers_active(sp)))
		return; /* Leakage unless caller handles error. */
	free_percpu(sp->per_cpu_ref);
	sp->per_cpu_ref = NULL;
}
EXPORT_SYMBOL_GPL(cleanup_srcu_struct);

static void srcu_gp_accumref(struct rcu_head *rcu);
static void srcu_gp_finish(struct srcu_struct *sp);
static void wakeme_after_rcu(struct rcu_head *head);

/*
 * The ->gp_ref is zero when the GP is initialized, but it is only
 * decreased except srcu_gp_accumref(), so if the ->gp_ref returns
 * to zero again after srcu_gp_accumref(), the GP is finished.
 */
void __srcu_read_unlock_special(struct srcu_struct *sp)
{
	if (atomic_dec_return(&sp->gp_ref) == 0)
		srcu_gp_finish(sp);
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock_special);

/*
 * Before srcu_gp_new(), all ongoing srcu-readers' idx SHOULD equal to
 * sp->completed & 0x1.
 */
static void srcu_gp_new(struct srcu_struct *sp, bool start)
{
	rcu_batch_move(&sp->batch_wait_gp, &sp->batch_queue);
	sp->completed++;
	if (start)
		call_rcu_sched(&sp->rcu, srcu_gp_accumref);
}

/*
 * Read sites' accesses to @sp are protected by rcu_read_lock_sched(),
 * and srcu_gp_accumref() is called after a rcu-sched-GP which is started
 * when starting this srcu-GP.
 *
 * With the help from this rcu-sched-GP, all srcu-readers will see
 * new sp->completed only, and ->c[old_idx] can't be changed any more
 * and srcu_gp_accumref() sees all updated ->c[old_idx] for accumulating
 * reference.
 *
 * And with the help from the rcu-sched-GP, all references of all
 * old srcu-readers(which idx==old_idx) are accumulated, so there is
 * no old srcu-reader(which idx==old_idx) left after this srcu-GP,
 * which matches the srcu_gp_new()'s requirement for next srcu-GP.
 *
 * Memory barrier/order:
 * Kind 1:
 *        CPU1                  CPU2
 *                      srcu-readers(fast path)
 * ------------------------------------------------
 *                              rcu-sched-GP ensures this order
 * ------------------------------------------------
 * code after srcu-GP
 *
 * Kind 2:
 *        CPU1                  CPU2
 *                      srcu-readers(slow path)
 *                        __srcu_read_unlock_special()
 * ------------------------------------------------
 *                              atomic operations ensure this order
 * ------------------------------------------------
 * code after srcu-GP
 */
static void srcu_gp_accumref(struct rcu_head *rcu)
{
	struct srcu_struct *sp = container_of(rcu, struct srcu_struct, rcu);
	int old_idx = (sp->completed & 0x1) ^ 0x1;
	int cpu;
	int count = 0;

	for_each_possible_cpu(cpu) {
		count += per_cpu_ptr(sp->per_cpu_ref, cpu)->c[old_idx];
		per_cpu_ptr(sp->per_cpu_ref, cpu)->c[old_idx] = 0;
	}

	if (atomic_add_return(count, &sp->gp_ref) == 0)
		srcu_gp_finish(sp);
}

/*
 * The GP is finish, schedule a kworker to process the callbacks,
 * (or directly invoke the callback if it is the only synchronize_srcu()
 * callback)
 *
 * And start a new GP when there is/are callback(s) pending.
 */
static void srcu_gp_finish(struct srcu_struct *sp)
{
	unsigned long flags;

	spin_lock_irqsave(&sp->lock, flags);
	if (likely(rcu_batch_is_singular(&sp->batch_wait_gp) &&
		   sp->batch_wait_gp.head->func == wakeme_after_rcu)) {
		wakeme_after_rcu(rcu_batch_dequeue(&sp->batch_wait_gp));
	} else {
		rcu_batch_move(&sp->batch_done, &sp->batch_wait_gp);
		schedule_work(&sp->work);
	}

	if (!rcu_batch_empty(&sp->batch_queue))
		srcu_gp_new(sp, true);
	spin_unlock_irqrestore(&sp->lock, flags);
}

/*
 * Process srcu callbacks from kworker.
 */
void process_srcu(struct work_struct *work)
{
	struct srcu_struct *sp;
	struct rcu_batch callbacks;
	struct rcu_head *head;

	sp = container_of(work, struct srcu_struct, work);
	rcu_batch_init(&callbacks);

	spin_lock_irq(&sp->lock);
	rcu_batch_move(&callbacks, &sp->batch_done);
	spin_unlock_irq(&sp->lock);

	for (;;) {
		head = rcu_batch_dequeue(&callbacks);
		if (!head)
			break;
		local_bh_disable();
		head->func(head);
		local_bh_enable();
	}
}
EXPORT_SYMBOL_GPL(process_srcu);

/*
 * Return true iff it starts a new GP or there is a existed GP running.
 * The caller should start the just created GP when return false.
 */
static bool __call_srcu(struct srcu_struct *sp, struct rcu_head *head,
		void (*func)(struct rcu_head *head), bool start)
{
	unsigned long flags;

	head->next = NULL;
	head->func = func;
	spin_lock_irqsave(&sp->lock, flags);
	rcu_batch_queue(&sp->batch_queue, head);
	if (rcu_batch_empty(&sp->batch_wait_gp))
		srcu_gp_new(sp, start);
	else
		start = true;
	spin_unlock_irqrestore(&sp->lock, flags);

	return start;
}

/*
 * Enqueue an SRCU callback on the specified srcu_struct structure,
 * start grace-period processing if it is not already running.
 */
void call_srcu(struct srcu_struct *sp, struct rcu_head *head,
		void (*func)(struct rcu_head *head))
{
	__call_srcu(sp, head, func, true);
}
EXPORT_SYMBOL_GPL(call_srcu);

struct rcu_synchronize {
	struct rcu_head head;
	struct completion completion;
};

/*
 * Awaken the corresponding synchronize_srcu() instance now that a
 * grace period has elapsed.
 */
static void wakeme_after_rcu(struct rcu_head *head)
{
	struct rcu_synchronize *rcu;

	rcu = container_of(head, struct rcu_synchronize, head);
	complete(&rcu->completion);
}

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
static void __synchronize_srcu(struct srcu_struct *sp, bool exp)
{
	struct rcu_synchronize rcu;

	rcu_lockdep_assert(!lock_is_held(&sp->dep_map) &&
			   !lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_srcu() in same-type SRCU (or RCU) read-side critical section");

	might_sleep();
	init_completion(&rcu.completion);

	if (!__call_srcu(sp, &rcu.head, wakeme_after_rcu, !exp)) {
		synchronize_sched_expedited();
		srcu_gp_accumref(&sp->rcu);
	}

	wait_for_completion(&rcu.completion);
}

/**
 * synchronize_srcu - wait for prior SRCU read-side critical-section completion
 * @sp: srcu_struct with which to synchronize.
 *
 * Can block; must be called from process context.
 *
 * Note that it is illegal to call synchronize_srcu() from the corresponding
 * SRCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_srcu() on one
 * srcu_struct from some other srcu_struct's read-side critical section.
 */
void synchronize_srcu(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, !!rcu_expedited);
}
EXPORT_SYMBOL_GPL(synchronize_srcu);

/**
 * synchronize_srcu_expedited - Brute-force SRCU grace period
 * @sp: srcu_struct with which to synchronize.
 *
 * Wait for an SRCU grace period to elapse, but be more aggressive about
 * spinning rather than blocking when waiting.
 *
 * Note that it is also illegal to call synchronize_srcu_expedited()
 * from the corresponding SRCU read-side critical section;
 * doing so will result in deadlock.  However, it is perfectly legal
 * to call synchronize_srcu_expedited() on one srcu_struct from some
 * other srcu_struct's read-side critical section, as long as
 * the resulting graph of srcu_structs is acyclic.
 */
void synchronize_srcu_expedited(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, true);
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

/**
 * srcu_barrier - Wait until all in-flight call_srcu() callbacks complete.
 */
void srcu_barrier(struct srcu_struct *sp)
{
	/* flush all queued callbacks to ->batch_done */
	synchronize_srcu(sp);
	/* flush all callbacks which may be being processed by kworker */
	flush_work(&sp->work);
}
EXPORT_SYMBOL_GPL(srcu_barrier);

/**
 * srcu_batches_completed - return batches completed.
 * @sp: srcu_struct on which to report batch completion.
 *
 * Report the number of batches, correlated with, but not necessarily
 * precisely the same as, the number of grace periods that have elapsed.
 */
long srcu_batches_completed(struct srcu_struct *sp)
{
	return sp->completed;
}
EXPORT_SYMBOL_GPL(srcu_batches_completed);
