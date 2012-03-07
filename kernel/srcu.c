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
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		Documentation/RCU/ *.txt
 *
 */

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/srcu.h>

static inline void rcu_batch_init(struct rcu_batch *b)
{
	b->head = NULL;
	b->tail = &b->head;
}

static inline void rcu_batch_queue(struct rcu_batch *b, struct rcu_head *head)
{
	*b->tail = head;
	b->tail = &head->next;
}

static inline bool rcu_batch_empty(struct rcu_batch *b)
{
	return b->tail == &b->head;
}

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

static inline void rcu_batch_move(struct rcu_batch *to, struct rcu_batch *from)
{
	if (!rcu_batch_empty(from)) {
		*to->tail = from->head;
		to->tail = from->tail;
		rcu_batch_init(from);
	}
}

/* single-thread state-machine */
static void process_srcu(struct work_struct *work);

static int init_srcu_struct_fields(struct srcu_struct *sp)
{
	sp->completed = 0;
	spin_lock_init(&sp->queue_lock);
	sp->running = false;
	rcu_batch_init(&sp->batch_queue);
	rcu_batch_init(&sp->batch_check0);
	rcu_batch_init(&sp->batch_check1);
	rcu_batch_init(&sp->batch_done);
	INIT_DELAYED_WORK(&sp->work, process_srcu);
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

/*
 * Returns approximate number of readers active on the specified rank
 * of per-CPU counters.  Also snapshots each counter's value in the
 * corresponding element of sp->snap[] for later use validating
 * the sum.
 */
static unsigned long srcu_readers_active_idx(struct srcu_struct *sp, int idx)
{
	int cpu;
	unsigned long sum = 0;
	unsigned long t;

	for_each_possible_cpu(cpu) {
		t = ACCESS_ONCE(per_cpu_ptr(sp->per_cpu_ref, cpu)->c[idx]);
		sum += t;
		sp->snap[cpu] = t;
	}
	return sum & SRCU_REF_MASK;
}

/*
 * To be called from the update side after an index flip.  Returns true
 * if the modulo sum of the counters is stably zero, false if there is
 * some possibility of non-zero.
 */
static bool srcu_readers_active_idx_check(struct srcu_struct *sp, int idx)
{
	int cpu;

	/*
	 * Note that srcu_readers_active_idx() can incorrectly return
	 * zero even though there is a pre-existing reader throughout.
	 * To see this, suppose that task A is in a very long SRCU
	 * read-side critical section that started on CPU 0, and that
	 * no other reader exists, so that the modulo sum of the counters
	 * is equal to one.  Then suppose that task B starts executing
	 * srcu_readers_active_idx(), summing up to CPU 1, and then that
	 * task C starts reading on CPU 0, so that its increment is not
	 * summed, but finishes reading on CPU 2, so that its decrement
	 * -is- summed.  Then when task B completes its sum, it will
	 * incorrectly get zero, despite the fact that task A has been
	 * in its SRCU read-side critical section the whole time.
	 *
	 * We therefore do a validation step should srcu_readers_active_idx()
	 * return zero.
	 */
	if (srcu_readers_active_idx(sp, idx) != 0)
		return false;

	/*
	 * Since the caller recently flipped ->completed, we can see at
	 * most one increment of each CPU's counter from this point
	 * forward.  The reason for this is that the reader CPU must have
	 * fetched the index before srcu_readers_active_idx checked
	 * that CPU's counter, but not yet incremented its counter.
	 * Its eventual counter increment will follow the read in
	 * srcu_readers_active_idx(), and that increment is immediately
	 * followed by smp_mb() B.  Because smp_mb() D is between
	 * the ->completed flip and srcu_readers_active_idx()'s read,
	 * that CPU's subsequent load of ->completed must see the new
	 * value, and therefore increment the counter in the other rank.
	 */
	smp_mb(); /* A */

	/*
	 * Now, we check the ->snap array that srcu_readers_active_idx()
	 * filled in from the per-CPU counter values. Since
	 * __srcu_read_lock() increments the upper bits of the per-CPU
	 * counter, an increment/decrement pair will change the value
	 * of the counter.  Since there is only one possible increment,
	 * the only way to wrap the counter is to have a huge number of
	 * counter decrements, which requires a huge number of tasks and
	 * huge SRCU read-side critical-section nesting levels, even on
	 * 32-bit systems.
	 *
	 * All of the ways of confusing the readings require that the scan
	 * in srcu_readers_active_idx() see the read-side task's decrement,
	 * but not its increment.  However, between that decrement and
	 * increment are smb_mb() B and C.  Either or both of these pair
	 * with smp_mb() A above to ensure that the scan below will see
	 * the read-side tasks's increment, thus noting a difference in
	 * the counter values between the two passes.
	 *
	 * Therefore, if srcu_readers_active_idx() returned zero, and
	 * none of the counters changed, we know that the zero was the
	 * correct sum.
	 *
	 * Of course, it is possible that a task might be delayed
	 * for a very long time in __srcu_read_lock() after fetching
	 * the index but before incrementing its counter.  This
	 * possibility will be dealt with in __synchronize_srcu().
	 */
	for_each_possible_cpu(cpu)
		if (sp->snap[cpu] !=
		    ACCESS_ONCE(per_cpu_ptr(sp->per_cpu_ref, cpu)->c[idx]))
			return false;  /* False zero reading! */
	return true;
}

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
	return sum & SRCU_REF_MASK;
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
	int sum;

	sum = srcu_readers_active(sp);
	WARN_ON(sum);  /* Leakage unless caller handles error. */
	if (sum != 0)
		return;
	free_percpu(sp->per_cpu_ref);
	sp->per_cpu_ref = NULL;
}
EXPORT_SYMBOL_GPL(cleanup_srcu_struct);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * srcu_struct.  Must be called from process context.
 * Returns an index that must be passed to the matching srcu_read_unlock().
 */
int __srcu_read_lock(struct srcu_struct *sp)
{
	int idx;

	preempt_disable();
	idx = rcu_dereference_index_check(sp->completed,
					  rcu_read_lock_sched_held()) & 0x1;
	ACCESS_ONCE(this_cpu_ptr(sp->per_cpu_ref)->c[idx]) +=
		SRCU_USAGE_COUNT + 1;
	smp_mb(); /* B */  /* Avoid leaking the critical section. */
	preempt_enable();
	return idx;
}
EXPORT_SYMBOL_GPL(__srcu_read_lock);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the srcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding srcu_read_lock().
 * Must be called from process context.
 */
void __srcu_read_unlock(struct srcu_struct *sp, int idx)
{
	preempt_disable();
	smp_mb(); /* C */  /* Avoid leaking the critical section. */
	ACCESS_ONCE(this_cpu_ptr(sp->per_cpu_ref)->c[idx]) -= 1;
	preempt_enable();
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock);

/*
 * We use an adaptive strategy for synchronize_srcu() and especially for
 * synchronize_srcu_expedited().  We spin for a fixed time period
 * (defined below) to allow SRCU readers to exit their read-side critical
 * sections.  If there are still some readers after 10 microseconds,
 * we repeatedly block for 1-millisecond time periods.  This approach
 * has done well in testing, so there is no need for a config parameter.
 */
#define SRCU_RETRY_CHECK_DELAY	5

static bool try_check_zero(struct srcu_struct *sp, int idx, int trycount)
{
	/*
	 * If a reader fetches the index before the ->completed increment,
	 * but increments its counter after srcu_readers_active_idx_check()
	 * sums it, then smp_mb() D will pair with __srcu_read_lock()'s
	 * smp_mb() B to ensure that the SRCU read-side critical section
	 * will see any updates that the current task performed before its
	 * call to synchronize_srcu(), or to synchronize_srcu_expedited(),
	 * as the case may be.
	 */
	smp_mb(); /* D */

	for (;;) {
		if (srcu_readers_active_idx_check(sp, idx))
			break;
		if (--trycount <= 0)
			return false;
		udelay(SRCU_RETRY_CHECK_DELAY);
	}

	/*
	 * The following smp_mb() E pairs with srcu_read_unlock()'s
	 * smp_mb C to ensure that if srcu_readers_active_idx_check()
	 * sees srcu_read_unlock()'s counter decrement, then any
	 * of the current task's subsequent code will happen after
	 * that SRCU read-side critical section.
	 *
	 * It also ensures the order between the above waiting and
	 * the next flipping.
	 */
	smp_mb(); /* E */

	return true;
}

/*
 * Flip the readers' index by incrementing ->completed, then new
 * readers will use counters referenced on new index value.
 */
static void srcu_flip(struct srcu_struct *sp)
{
	ACCESS_ONCE(sp->completed)++;
}

void call_srcu(struct srcu_struct *sp, struct rcu_head *head,
		void (*func)(struct rcu_head *head))
{
	unsigned long flags;

	head->next = NULL;
	head->func = func;
	spin_lock_irqsave(&sp->queue_lock, flags);
	rcu_batch_queue(&sp->batch_queue, head);
	if (!sp->running) {
		sp->running = true;
		queue_delayed_work(system_nrt_wq, &sp->work, 0);
	}
	spin_unlock_irqrestore(&sp->queue_lock, flags);
}
EXPORT_SYMBOL_GPL(call_srcu);

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
static void __synchronize_srcu(struct srcu_struct *sp)
{
	rcu_lockdep_assert(!lock_is_held(&sp->dep_map) &&
			   !lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_srcu() in same-type SRCU (or RCU) read-side critical section");

	__wait_srcu_gp(sp, call_srcu);
}

/**
 * synchronize_srcu - wait for prior SRCU read-side critical-section completion
 * @sp: srcu_struct with which to synchronize.
 *
 * Flip the completed counter, and wait for the old count to drain to zero.
 * As with classic RCU, the updater must use some separate means of
 * synchronizing concurrent updates.  Can block; must be called from
 * process context.
 *
 * Note that it is illegal to call synchronize_srcu() from the corresponding
 * SRCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_srcu() on one
 * srcu_struct from some other srcu_struct's read-side critical section.
 */
void synchronize_srcu(struct srcu_struct *sp)
{
	__synchronize_srcu(sp);
}
EXPORT_SYMBOL_GPL(synchronize_srcu);

/**
 * synchronize_srcu_expedited - Brute-force SRCU grace period
 * @sp: srcu_struct with which to synchronize.
 *
 * Wait for an SRCU grace period to elapse, but be more aggressive about
 * spinning rather than blocking when waiting.
 *
 * Note that it is illegal to call this function while holding any lock
 * that is acquired by a CPU-hotplug notifier.  It is also illegal to call
 * synchronize_srcu_expedited() from the corresponding SRCU read-side
 * critical section; doing so will result in deadlock.  However, it is
 * perfectly legal to call synchronize_srcu_expedited() on one srcu_struct
 * from some other srcu_struct's read-side critical section, as long as
 * the resulting graph of srcu_structs is acyclic.
 */
void synchronize_srcu_expedited(struct srcu_struct *sp)
{
	__synchronize_srcu(sp);
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

void srcu_barrier(struct srcu_struct *sp)
{
	__synchronize_srcu(sp);
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

#define SRCU_CALLBACK_BATCH	10
#define SRCU_INTERVAL		1

static void srcu_collect_new(struct srcu_struct *sp)
{
	if (!rcu_batch_empty(&sp->batch_queue)) {
		spin_lock_irq(&sp->queue_lock);
		rcu_batch_move(&sp->batch_check0, &sp->batch_queue);
		spin_unlock_irq(&sp->queue_lock);
	}
}

static void srcu_advance_batches(struct srcu_struct *sp)
{
	int idx = 1 - (sp->completed & 0x1UL);

	/*
	 * SRCU read-side critical sections are normally short, so check
	 * twice after a flip.
	 */
	if (!rcu_batch_empty(&sp->batch_check1) ||
	    !rcu_batch_empty(&sp->batch_check0)) {
		if (try_check_zero(sp, idx, 1)) {
			rcu_batch_move(&sp->batch_done, &sp->batch_check1);
			rcu_batch_move(&sp->batch_check1, &sp->batch_check0);
			if (!rcu_batch_empty(&sp->batch_check1)) {
				srcu_flip(sp);
				if (try_check_zero(sp, 1 - idx, 2)) {
					rcu_batch_move(&sp->batch_done,
						&sp->batch_check1);
				}
			}
		}
	}
}

static void srcu_invoke_callbacks(struct srcu_struct *sp)
{
	int i;
	struct rcu_head *head;

	for (i = 0; i < SRCU_CALLBACK_BATCH; i++) {
		head = rcu_batch_dequeue(&sp->batch_done);
		if (!head)
			break;
		head->func(head);
	}
}

static void srcu_reschedule(struct srcu_struct *sp)
{
	bool running = true;

	if (rcu_batch_empty(&sp->batch_done) &&
	    rcu_batch_empty(&sp->batch_check1) &&
	    rcu_batch_empty(&sp->batch_check0) &&
	    rcu_batch_empty(&sp->batch_queue)) {
		spin_lock_irq(&sp->queue_lock);
		if (rcu_batch_empty(&sp->batch_queue)) {
			sp->running = false;
			running = false;
		}
		spin_unlock_irq(&sp->queue_lock);
	}

	if (running)
		queue_delayed_work(system_nrt_wq, &sp->work, SRCU_INTERVAL);
}

static void process_srcu(struct work_struct *work)
{
	struct srcu_struct *sp;

	sp = container_of(work, struct srcu_struct, work.work);

	srcu_collect_new(sp);
	srcu_advance_batches(sp);
	srcu_invoke_callbacks(sp);
	srcu_reschedule(sp);
}
