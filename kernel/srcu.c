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
#include <linux/completion.h>

#define SRCU_CALLBACK_BATCH	10
#define SRCU_INTERVAL		1

/* protected by sp->gp_lock */
struct srcu_cpu_struct {
	/* callback queue for handling */
	struct srcu_head *head, **tail;

	/* the struct srcu_struct of this struct srcu_cpu_struct */
	struct srcu_struct *sp;
	struct delayed_work work;
};

/*
 * State machine process for every CPU, it may run on wrong CPU
 * during hotplugging or synchronize_srcu() scheldule it after
 * migrated.
 */
static void process_srcu_cpu_struct(struct work_struct *work);

static struct workqueue_struct *srcu_callback_wq;

static int init_srcu_struct_fields(struct srcu_struct *sp)
{
	int cpu;
	struct srcu_cpu_struct *scp;

	mutex_init(&sp->flip_check_mutex);
	spin_lock_init(&sp->gp_lock);
	sp->completed = 0;
	sp->chck_seq = 0;
	sp->callback_chck_seq = 0;
	sp->zero_seq[0] = 0;
	sp->zero_seq[1] = 0;

	sp->per_cpu_ref = alloc_percpu(struct srcu_struct_array);
	if (!sp->per_cpu_ref)
		return -ENOMEM;

	sp->srcu_per_cpu = alloc_percpu(struct srcu_cpu_struct);
	if (!sp->srcu_per_cpu) {
		free_percpu(sp->per_cpu_ref);
		return -ENOMEM;
	}

	for_each_possible_cpu(cpu) {
		scp = per_cpu_ptr(sp->srcu_per_cpu, cpu);

		scp->sp = sp;
		scp->head = NULL;
		scp->tail = &scp->head;
		INIT_DELAYED_WORK(&scp->work, process_srcu_cpu_struct);
	}

	return 0;
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
	free_percpu(sp->srcu_per_cpu);
	sp->srcu_per_cpu = NULL;
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
 * 'return left < right;' but handle the overflow issues.
 * The same as 'return (long)(right - left) > 0;' but it cares more.
 */
static inline
bool safe_less_than(unsigned long left, unsigned long right, unsigned long max)
{
	unsigned long a = right - left;
	unsigned long b = max - left;

	return !!a && (a <= b);
}

/*
 * We use an adaptive strategy for synchronize_srcu() and especially for
 * synchronize_srcu_expedited().  We spin for a fixed time period
 * (defined below) to allow SRCU readers to exit their read-side critical
 * sections.  If there are still some readers after 10 microseconds,
 * we repeatedly block for 1-millisecond time periods.  This approach
 * has done well in testing, so there is no need for a config parameter.
 */
#define SRCU_RETRY_CHECK_DELAY		5
#define SYNCHRONIZE_SRCU_TRYCOUNT	2
#define SYNCHRONIZE_SRCU_EXP_TRYCOUNT	12

static bool do_check_zero_idx(struct srcu_struct *sp, int idx, int trycount)
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

/* Must called with sp->flip_check_mutex and sp->gp_lock held; */
static bool check_zero_idx(struct srcu_struct *sp, int idx,
		struct srcu_head *head, int trycount)
{
	unsigned long chck_seq;
	bool checked_zero;

	/* find out the check sequence number for this check */
	if (sp->chck_seq == sp->callback_chck_seq ||
	    sp->chck_seq == head->chck_seq)
		sp->chck_seq++;
	chck_seq = sp->chck_seq;

	spin_unlock_irq(&sp->gp_lock);
	checked_zero = do_check_zero_idx(sp, idx, trycount);
	spin_lock_irq(&sp->gp_lock);

	if (!checked_zero)
		return false;

	/* commit the succeed check */
	sp->zero_seq[idx] = chck_seq;

	return true;
}

/*
 * Are the @head completed? will try do check zero when it is not.
 *
 * Must be called with sp->flip_check_mutex and sp->gp_lock held;
 * Must be called from process contex, because the check may be long
 * The sp->gp_lock may be released and regained.
 */
static bool complete_head_flip_check(struct srcu_struct *sp,
		struct srcu_head *head, int trycount)
{
	int idxb = sp->completed & 0X1UL;
	int idxa = 1 - idxb;
	unsigned long h  = head->chck_seq;
	unsigned long za = sp->zero_seq[idxa];
	unsigned long zb = sp->zero_seq[idxb];
	unsigned long s  = sp->chck_seq;

	if (!safe_less_than(h, za, s)) {
		if (!check_zero_idx(sp, idxa, head, trycount))
			return false;
	}

	if (!safe_less_than(h, zb, s)) {
		srcu_flip(sp);
		trycount = trycount < 2 ? 2 : trycount;
		return check_zero_idx(sp, idxb, head, trycount);
	}

	return true;
}

/*
 * Are the @head completed?
 *
 * Must called with sp->gp_lock held;
 * srcu_queue_callback() and check_zero_idx() ensure (s - z0) and (s - z1)
 * less than (ULONG_MAX / sizof(struct srcu_head)). There is at least one
 * callback queued for each seq in (z0, s) and (z1, s). The same for
 * za, zb, s in complete_head_flip_check().
 */
static bool complete_head(struct srcu_struct *sp, struct srcu_head *head)
{
	unsigned long h  = head->chck_seq;
	unsigned long z0 = sp->zero_seq[0];
	unsigned long z1 = sp->zero_seq[1];
	unsigned long s  = sp->chck_seq;

	return safe_less_than(h, z0, s) && safe_less_than(h, z1, s);
}

static void process_srcu_cpu_struct(struct work_struct *work)
{
	int i;
	int can_flip_check;
	struct srcu_head *head;
	struct srcu_cpu_struct *scp;
	struct srcu_struct *sp;
	work_func_t wfunc;

	scp = container_of(work, struct srcu_cpu_struct, work.work);
	sp = scp->sp;

	can_flip_check = mutex_trylock(&sp->flip_check_mutex);
	spin_lock_irq(&sp->gp_lock);

	for (i = 0; i < SRCU_CALLBACK_BATCH; i++) {
		head = scp->head;
		if (!head)
			break;

		/* Test whether the head is completed or not. */
		if (can_flip_check) {
			if (!complete_head_flip_check(sp, head, 1))
				break;
		} else {
			if (!complete_head(sp, head))
				break;
		}

		/* dequeue the completed callback */
		scp->head = head->next;
		if (!scp->head)
			scp->tail = &scp->head;

		/* deliver the callback, will be invoked in workqueue */
		BUILD_BUG_ON(offsetof(struct srcu_head, work) != 0);
		wfunc = (work_func_t)head->func;
		INIT_WORK(&head->work, wfunc);
		queue_work(srcu_callback_wq, &head->work);
	}
	if (scp->head)
		schedule_delayed_work(&scp->work, SRCU_INTERVAL);
	spin_unlock_irq(&sp->gp_lock);
	if (can_flip_check)
		mutex_unlock(&sp->flip_check_mutex);
}

static
void srcu_queue_callback(struct srcu_struct *sp, struct srcu_cpu_struct *scp,
                struct srcu_head *head, srcu_callback_func_t func)
{
	head->next = NULL;
	head->func = func;
	head->chck_seq = sp->chck_seq;
	sp->callback_chck_seq = sp->chck_seq;
	*scp->tail = head;
	scp->tail = &head->next;
}

void call_srcu(struct srcu_struct *sp, struct srcu_head *head,
		srcu_callback_func_t func)
{
	unsigned long flags;
	int cpu = get_cpu();
	struct srcu_cpu_struct *scp = per_cpu_ptr(sp->srcu_per_cpu, cpu);

	spin_lock_irqsave(&sp->gp_lock, flags);
	srcu_queue_callback(sp, scp, head, func);
	/* start state machine when this is the head */
	if (scp->head == head)
		schedule_delayed_work(&scp->work, 0);
	spin_unlock_irqrestore(&sp->gp_lock, flags);
	put_cpu();
}
EXPORT_SYMBOL_GPL(call_srcu);

struct srcu_sync {
	struct srcu_head head;
	struct completion completion;
};

static void __synchronize_srcu_callback(struct srcu_head *head)
{
	struct srcu_sync *sync = container_of(head, struct srcu_sync, head);

	complete(&sync->completion);
}

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
static void __synchronize_srcu(struct srcu_struct *sp, int try_count)
{
	struct srcu_sync sync;
	struct srcu_head *head = &sync.head;
	struct srcu_head **orig_tail;
	int cpu = raw_smp_processor_id();
	struct srcu_cpu_struct *scp = per_cpu_ptr(sp->srcu_per_cpu, cpu);
	bool started;

	rcu_lockdep_assert(!lock_is_held(&sp->dep_map) &&
			   !lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_srcu() in same-type SRCU (or RCU) read-side critical section");

	init_completion(&sync.completion);
	if (!mutex_trylock(&sp->flip_check_mutex)) {
		call_srcu(sp, &sync.head, __synchronize_srcu_callback);
		goto wait;
	}

	spin_lock_irq(&sp->gp_lock);
	started = !!scp->head;
	orig_tail = scp->tail;
	srcu_queue_callback(sp, scp, head, __synchronize_srcu_callback);

	/* fast path */
	if (complete_head_flip_check(sp, head, try_count)) {
		/*
		 * dequeue @head, we hold flip_check_mutex, the previous
		 * node stays or all prevous node are all dequeued.
		 */
		if (scp->head == head)
			orig_tail = &scp->head;
		*orig_tail = head->next;
		if (*orig_tail == NULL)
			scp->tail = orig_tail;

		/*
		 * start state machine if this is the head and some callback(s)
		 * are queued when we do check_zero(they have not started it).
		 */
		if (!started && scp->head)
			schedule_delayed_work(&scp->work, 0);

		/* we done! */
		spin_unlock_irq(&sp->gp_lock);
		mutex_unlock(&sp->flip_check_mutex);
		return;
	}

	/* slow path */

	/* start state machine when this is the head */
	if (!started)
		schedule_delayed_work(&scp->work, 0);
	spin_unlock_irq(&sp->gp_lock);
	mutex_unlock(&sp->flip_check_mutex);

wait:
	wait_for_completion(&sync.completion);
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
	__synchronize_srcu(sp, SYNCHRONIZE_SRCU_TRYCOUNT);
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
	__synchronize_srcu(sp, SYNCHRONIZE_SRCU_EXP_TRYCOUNT);
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

void srcu_barrier(struct srcu_struct *sp)
{
	struct srcu_sync sync;
	struct srcu_head *head = &sync.head;
	unsigned long chck_seq; /* snap */

	int idle_loop = 0;
	int cpu;
	struct srcu_cpu_struct *scp;

	spin_lock_irq(&sp->gp_lock);
	chck_seq = sp->chck_seq;
	for_each_possible_cpu(cpu) {
		scp = per_cpu_ptr(sp->srcu_per_cpu, cpu);
		if (scp->head && !safe_less_than(chck_seq, scp->head->chck_seq,
				sp->chck_seq)) {
			/* this path is likely enterred only once */
			init_completion(&sync.completion);
			srcu_queue_callback(sp, scp, head,
					__synchronize_srcu_callback);
			/* don't need to wakeup the woken state machine */
			spin_unlock_irq(&sp->gp_lock);
			wait_for_completion(&sync.completion);
			spin_lock_irq(&sp->gp_lock);
		} else {
			if ((++idle_loop & 0xF) == 0) {
				spin_unlock_irq(&sp->gp_lock);
				udelay(1);
				spin_lock_irq(&sp->gp_lock);
			}
		}
	}
	spin_unlock_irq(&sp->gp_lock);

	flush_workqueue(srcu_callback_wq);
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

__init int srcu_init(void)
{
	srcu_callback_wq = alloc_workqueue("srcu", 0, 0);
	return srcu_callback_wq ? 0 : -1;
}
subsys_initcall(srcu_init);
