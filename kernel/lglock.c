/* See include/linux/lglock.h for description */
#include <linux/module.h>
#include <linux/lglock.h>
#include <linux/cpu.h>
#include <linux/string.h>

/*
 * Note there is no uninit, so lglocks cannot be defined in
 * modules (but it's fine to use them from there)
 * Could be added though, just undo lg_lock_init
 */

void lg_lock_init(struct lglock *lg, char *name)
{
	LOCKDEP_INIT_MAP(&lg->lock_dep_map, name, &lg->lock_key, 0);
}
EXPORT_SYMBOL(lg_lock_init);

void lg_local_lock(struct lglock *lg)
{
	arch_spinlock_t *lock;

	preempt_disable();
	rwlock_acquire_read(&lg->lock_dep_map, 0, 0, _RET_IP_);
	lock = this_cpu_ptr(lg->lock);
	arch_spin_lock(lock);
}
EXPORT_SYMBOL(lg_local_lock);

void lg_local_unlock(struct lglock *lg)
{
	arch_spinlock_t *lock;

	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	lock = this_cpu_ptr(lg->lock);
	arch_spin_unlock(lock);
	preempt_enable();
}
EXPORT_SYMBOL(lg_local_unlock);

void lg_local_lock_cpu(struct lglock *lg, int cpu)
{
	arch_spinlock_t *lock;

	preempt_disable();
	rwlock_acquire_read(&lg->lock_dep_map, 0, 0, _RET_IP_);
	lock = per_cpu_ptr(lg->lock, cpu);
	arch_spin_lock(lock);
}
EXPORT_SYMBOL(lg_local_lock_cpu);

void lg_local_unlock_cpu(struct lglock *lg, int cpu)
{
	arch_spinlock_t *lock;

	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	lock = per_cpu_ptr(lg->lock, cpu);
	arch_spin_unlock(lock);
	preempt_enable();
}
EXPORT_SYMBOL(lg_local_unlock_cpu);

void lg_global_lock(struct lglock *lg)
{
	int i;

	preempt_disable();
	rwlock_acquire(&lg->lock_dep_map, 0, 0, _RET_IP_);
	for_each_possible_cpu(i) {
		arch_spinlock_t *lock;
		lock = per_cpu_ptr(lg->lock, i);
		arch_spin_lock(lock);
	}
}
EXPORT_SYMBOL(lg_global_lock);

void lg_global_unlock(struct lglock *lg)
{
	int i;

	rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
	for_each_possible_cpu(i) {
		arch_spinlock_t *lock;
		lock = per_cpu_ptr(lg->lock, i);
		arch_spin_unlock(lock);
	}
	preempt_enable();
}
EXPORT_SYMBOL(lg_global_unlock);

#define FALLBACK_BASE	(1UL << 30)

void lg_rwlock_local_read_lock(struct lgrwlock *lgrw)
{
	struct lglock *lg = &lgrw->lglock;

	preempt_disable();
	if (likely(!__this_cpu_read(*lgrw->reader_refcnt))) {
		rwlock_acquire_read(&lg->lock_dep_map, 0, 0, _RET_IP_);
		if (unlikely(!arch_spin_trylock(this_cpu_ptr(lg->lock)))) {
			read_lock(&lgrw->fallback_rwlock);
			__this_cpu_write(*lgrw->reader_refcnt, FALLBACK_BASE);
			return;
		}
	}

	__this_cpu_inc(*lgrw->reader_refcnt);
}
EXPORT_SYMBOL(lg_rwlock_local_read_lock);

void lg_rwlock_local_read_unlock(struct lgrwlock *lgrw)
{
	switch (__this_cpu_read(*lgrw->reader_refcnt)) {
	case 1:
		__this_cpu_write(*lgrw->reader_refcnt, 0);
		lg_local_unlock(&lgrw->lglock);
		return;
	case FALLBACK_BASE:
		__this_cpu_write(*lgrw->reader_refcnt, 0);
		read_unlock(&lgrw->fallback_rwlock);
		rwlock_release(&lg->lock_dep_map, 1, _RET_IP_);
		break;
	default:
		__this_cpu_dec(*lgrw->reader_refcnt);
		break;
	}

	preempt_enable();
}
EXPORT_SYMBOL(lg_rwlock_local_read_unlock);

void lg_rwlock_global_write_lock(struct lgrwlock *lgrw)
{
	lg_global_lock(&lgrw->lglock);
	write_lock(&lgrw->fallback_rwlock);
}
EXPORT_SYMBOL(lg_rwlock_global_write_lock);

void lg_rwlock_global_write_unlock(struct lgrwlock *lgrw)
{
	write_unlock(&lgrw->fallback_rwlock);
	lg_global_unlock(&lgrw->lglock);
}
EXPORT_SYMBOL(lg_rwlock_global_write_unlock);

/* special write-site APIs allolw nested reader in such write-site */
void __lg_rwlock_global_read_write_lock(struct lgrwlock *lgrw)
{
	lg_rwlock_global_write_lock(lgrw);
	__this_cpu_write(*lgrw->reader_refcnt, 1);
}

void __lg_rwlock_global_read_write_unlock(struct lgrwlock *lgrw)
{
	__this_cpu_write(*lgrw->reader_refcnt, 0);
	lg_rwlock_global_write_unlock(lgrw);
}
