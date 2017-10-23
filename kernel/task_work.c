// SPDX-License-Identifier: GPL-2.0
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/tracehook.h>

//# 注册，删除task_work. task_work 在task返回userspace,exit时被调用
//# TODO 在 signal.c 也有两次调用 task_work_run 没理解， SIGSTOP 吗？
//# kernel/irq/manage.c 用于处理irq线程因不合理的dirver代码而退出的情况
//# kernel/sched/fair.c 将笨重代码从schedule() 那里挪出来
//# uprobe 用于刚创建的新进程的即将回到user时候做相关初始化
//# fs/namespace.c 用于delay mntput. 这个莫名其妙，跟userspace 又啥关系？
//# fs/file_table.c 用于delay put. 这个莫名其妙，跟userspace 又啥关系？
//# 应该是使用内核其它的delay 机制(比如workqueue)跟本task同步maf，这个刚好使用
//# intel rdt, 移动cpu rdt。好理解
//# security 2 处，不好理解

static struct callback_head work_exited; /* all we need is ->next == NULL */

/**
 * task_work_add - ask the @task to execute @work->func()
 * @task: the task which should run the callback
 * @work: the callback to run
 * @notify: send the notification if true
 *
 * Queue @work for task_work_run() below and notify the @task if @notify.
 * Fails if the @task is exiting/exited and thus it can't process this @work.
 * Otherwise @work->func() will be called when the @task returns from kernel
 * mode or exits.
 *
 * This is like the signal handler which runs in kernel mode, but it doesn't
 * try to wake up the @task.
 *
 * Note: there is no ordering guarantee on works queued here.
 *
 * RETURNS:
 * 0 if succeeds or -ESRCH.
 */
int
task_work_add(struct task_struct *task, struct callback_head *work, bool notify)
{
	struct callback_head *head;

	do {
		head = READ_ONCE(task->task_works);
		if (unlikely(head == &work_exited))
			return -ESRCH;
		work->next = head;
	} while (cmpxchg(&task->task_works, head, work) != head);

	if (notify)
		set_notify_resume(task);
	return 0;
}

/**
 * task_work_cancel - cancel a pending work added by task_work_add()
 * @task: the task which should execute the work
 * @func: identifies the work to remove
 *
 * Find the last queued pending work with ->func == @func and remove
 * it from queue.
 *
 * RETURNS:
 * The found work or NULL if not found.
 */
struct callback_head *
task_work_cancel(struct task_struct *task, task_work_func_t func)
{
	struct callback_head **pprev = &task->task_works;
	struct callback_head *work;
	unsigned long flags;

	if (likely(!task->task_works))
		return NULL;
	/*
	 * If cmpxchg() fails we continue without updating pprev.
	 * Either we raced with task_work_add() which added the
	 * new entry before this work, we will find it again. Or
	 * we raced with task_work_run(), *pprev == NULL/exited.
	 */
	raw_spin_lock_irqsave(&task->pi_lock, flags);
	while ((work = lockless_dereference(*pprev))) {
		if (work->func != func)
			pprev = &work->next;
		else if (cmpxchg(pprev, work, work->next) == work)
			break;
	}
	raw_spin_unlock_irqrestore(&task->pi_lock, flags);

	return work;
}

/**
 * task_work_run - execute the works added by task_work_add()
 *
 * Flush the pending works. Should be used by the core kernel code.
 * Called before the task returns to the user-mode or stops, or when
 * it exits. In the latter case task_work_add() can no longer add the
 * new work after task_work_run() returns.
 */
void task_work_run(void)
{
	struct task_struct *task = current;
	struct callback_head *work, *head, *next;

	for (;;) {
		/*
		 * work->func() can do task_work_add(), do not set
		 * work_exited unless the list is empty.
		 */
		raw_spin_lock_irq(&task->pi_lock);
		do {
			work = READ_ONCE(task->task_works);
			head = !work && (task->flags & PF_EXITING) ?
				&work_exited : NULL;
		} while (cmpxchg(&task->task_works, work, head) != work);
		raw_spin_unlock_irq(&task->pi_lock);

		if (!work)
			break;

		do {
			next = work->next;
			work->func(work);
			work = next;
			cond_resched();
		} while (work);
	}
}
