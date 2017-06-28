/* Copyright (c) 2013 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Plan9 style Rendezvous (http://plan9.bell-labs.com/sys/doc/sleep.html)
 *
 * We implement it with CVs, and it can handle multiple sleepers/wakers. */

#include <rendez.h>
#include <kthread.h>
#include <alarm.h>
#include <assert.h>
#include <smp.h>
#include <err.h>

void rendez_init(struct rendez *rv)
{
	cv_init_irqsave(&rv->cv);
}

// XXX can we have multiple sleepers on a rendez with different functions?
// 		if not, we can have the func (+ arg?) in the object
// 		waker can check the func too, to avoid spurious wakeups
//
// 		if so, we could still store the func with some lookup-elem and still
// 		have the waker run the funcs.
void rendez_sleep(struct rendez *rv, int (*cond)(void*), void *arg)
{
	int8_t irq_state = 0;
	struct cv_lookup_elm cle;
	/* Do a quick check before registering and sleeping.  this is the 'check,
	 * signal, check again' pattern, where the first check is an optimization.
	 * Many rendezes will already be satisfied, so we want to avoid excessive
	 * locking associated with reg/dereg. */
	cv_lock_irqsave(&rv->cv, &irq_state);
	if (cond(arg)) {
		cv_unlock_irqsave(&rv->cv, &irq_state);
		return;
	}
	__reg_abortable_cv(&cle, &rv->cv);
	/* Mesa-style semantics, which is definitely what you want.  See the
	 * discussion at the end of the URL above. */
	while (!cond(arg)) {
		/* it's okay if we miss the ABORT flag; we hold the cv lock, so an
		 * aborter's broadcast is waiting until we unlock. */
		if (should_abort(&cle)) {
			cv_unlock_irqsave(&rv->cv, &irq_state);
			dereg_abortable_cv(&cle);
			error(EINTR, "syscall aborted");
		}
		cv_wait(&rv->cv);
		cpu_relax();
	}
	cv_unlock_irqsave(&rv->cv, &irq_state);
	dereg_abortable_cv(&cle);
}

/* Force a wakeup of all waiters on the rv, including non-timeout users.  For
 * those, they will just wake up, see the condition is still false (probably)
 * and go back to sleep. */
static void rendez_alarm_handler(struct alarm_waiter *awaiter,
                                 struct hw_trapframe *hw_tf)
{
	struct rendez *rv = (struct rendez*)awaiter->data;

	rendez_wakeup(rv);
}

/* Like sleep, but it will timeout in 'usec' microseconds. */
void rendez_sleep_timeout(struct rendez *rv, int (*cond)(void*), void *arg,
                          uint64_t usec)
{
	int8_t irq_state = 0;
	struct alarm_waiter awaiter;
	struct cv_lookup_elm cle;
	struct timer_chain *pcpui_tchain = &per_cpu_info[core_id()].tchain;

	if (!usec)
		return;
	/* Doing this cond check early, but then unlocking again.  Mostly just to
	 * avoid weird issues with the CV lock and the alarm tchain lock. */
	cv_lock_irqsave(&rv->cv, &irq_state);
	if (cond(arg)) {
		cv_unlock_irqsave(&rv->cv, &irq_state);
		return;
	}
	cv_unlock_irqsave(&rv->cv, &irq_state);
	/* The handler will call rendez_wake, but won't mess with the condition
	 * state.  It's enough to break us out of cv_wait() to see .on_tchain.
	 * Since all we're doing is poking a rendez, we might as well just do it
	 * from IRQ ctx instead of mucking with an extra RKM.  It also avoids issues
	 * with unset_alarm blocking. */
	init_awaiter_irq(&awaiter, rendez_alarm_handler);
	awaiter.data = rv;
	set_awaiter_rel(&awaiter, usec);
	/* Set our alarm on this cpu's tchain.  Note that when we sleep in cv_wait,
	 * we could be migrated, and later on we could be unsetting the alarm
	 * remotely. */
	set_alarm(pcpui_tchain, &awaiter);
	cv_lock_irqsave(&rv->cv, &irq_state);
	__reg_abortable_cv(&cle, &rv->cv);
	/* We could wake early for a few reasons.  Legit wakeups after a changed
	 * condition (and we should exit), other alarms with different timeouts (and
	 * we should go back to sleep), etc.  Note it is possible for our alarm to
	 * fire immediately upon setting it: before we even cv_lock. */
	while (!cond(arg) && awaiter.on_tchain) {
		if (should_abort(&cle)) {
			cv_unlock_irqsave(&rv->cv, &irq_state);
			unset_alarm(pcpui_tchain, &awaiter);
			dereg_abortable_cv(&cle);
			error(EINTR, "syscall aborted");
		}
		cv_wait(&rv->cv);
		cpu_relax();
	}
	cv_unlock_irqsave(&rv->cv, &irq_state);
	dereg_abortable_cv(&cle);
	/* Turn off our alarm.  If it already fired, this is a no-op.  Note this
	 * could be cross-core. */
	unset_alarm(pcpui_tchain, &awaiter);
}

/* plan9 rendez returned a pointer to the proc woken up.  we return "true" if we
 * woke someone up. */
bool rendez_wakeup(struct rendez *rv)
{
	int8_t irq_state = 0;
	bool ret;
	/* The plan9 style "one sleeper, one waker" could get by with a signal here.
	 * But we want to make sure all potential waiters are woken up. */
	cv_lock_irqsave(&rv->cv, &irq_state);
	ret = rv->cv.nr_waiters ? TRUE : FALSE;
	__cv_broadcast(&rv->cv);
	cv_unlock_irqsave(&rv->cv, &irq_state);
	return ret;
}
