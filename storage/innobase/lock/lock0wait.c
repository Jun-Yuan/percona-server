/*****************************************************************************

Copyright (c) 1996, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file lock/lock0wait.c
The transaction lock system

Created 25/5/2010 Sunny Bains
*******************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "srv0mon.h"
#include "que0que.h"
#include "lock0lock.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "ha_prototypes.h"
#include "lock0priv.h"

UNIV_INTERN ibool	srv_lock_timeout_active 	= FALSE;
UNIV_INTERN ulint	srv_n_lock_wait_count		= 0;
UNIV_INTERN ulint	srv_n_lock_wait_current_count	= 0;
UNIV_INTERN ib_int64_t	srv_n_lock_wait_time		= 0;
UNIV_INTERN ulint	srv_n_lock_max_wait_time	= 0;

UNIV_INTERN os_event_t	srv_timeout_event;

/*********************************************************************//**
Print the contents of the lock_sys_t::waiting_threads array. */
static
void
lock_wait_table_print(void)
/*=======================*/
{
	ulint			i;
	const srv_slot_t*	slot;

	ut_ad(lock_wait_mutex_own());

	slot = lock_sys->waiting_threads;

	for (i = 0; i < OS_THREAD_MAX_N; i++, ++slot) {

		fprintf(stderr,
			"Slot %lu: thread id %lu, type %lu,"
			" in use %lu, susp %lu, time %lu\n",
			(ulong) i,
			(ulong) os_thread_pf(slot->id),
			(ulong) slot->type,
			(ulong) slot->in_use,
			(ulong) slot->suspended,
			(ulong) difftime(ut_time(), slot->suspend_time));
	}
}

/*********************************************************************//**
Release a slot in the lock_sys_t::waiting_threads. Adjust the array last pointer
if there are empty slots towards the end of the table. */
static
void
lock_wait_table_release_slot(
/*=========================*/
	srv_slot_t*	slot)		/*!< in: slot to release */
{
#ifdef UNIV_DEBUG
	srv_slot_t*	upper = lock_sys->waiting_threads + OS_THREAD_MAX_N;
#endif /* UNIV_DEBUG */

	lock_wait_mutex_enter();

	ut_ad(slot->in_use);
	ut_ad(slot->thr != NULL);
	ut_ad(slot->thr->slot != NULL);
	ut_ad(slot->thr->slot == slot);

	/* Must be within the array boundaries. */
	ut_ad(slot >= lock_sys->waiting_threads);
	ut_ad(slot < upper);

	/* Note: When we reserve the slot we use the trx_t::mutex to update
	the slot values to change the state to reserve. Here we are using the
	lock mutex to change the state of the slot to free. This is by design,
	because when we query the slot state we always hold both the lock and
	trx_t::mutex. To reduce contention on the lock mutex when reserving the
	slot we avoid acquiring the lock mutex. */

	lock_mutex_enter();

	slot->thr->slot = NULL;
	slot->thr = NULL;
	slot->in_use = FALSE;

	lock_mutex_exit();

	/* Scan backwards and adjust the last free slot pointer. */
	for (slot = lock_sys->last_slot;
	     slot > lock_sys->waiting_threads && !slot->in_use;
	     --slot) {
		/* No op */
	}

	/* Either the array is empty or the last scanned slot is in use. */
	ut_ad(slot->in_use || slot == lock_sys->waiting_threads);

	lock_sys->last_slot = slot + 1;

	/* The last slot is either outside of the array boundary or it's
	on an empty slot. */
	ut_ad(lock_sys->last_slot == upper || !lock_sys->last_slot->in_use);

	ut_ad(lock_sys->last_slot >= lock_sys->waiting_threads);
	ut_ad(lock_sys->last_slot <= upper);

	lock_wait_mutex_exit();
}

/*********************************************************************//**
Reserves a slot in the thread table for the current user OS thread.
@return	reserved slot */
static
srv_slot_t*
lock_wait_table_reserve_slot(
/*=========================*/
	que_thr_t*	thr)		/*!< in: query thread associated
					with the user OS thread */
{
	ulint		i;
	srv_slot_t*	slot;

	ut_ad(lock_wait_mutex_own());
	ut_ad(trx_mutex_own(thr_get_trx(thr)));

	slot = lock_sys->waiting_threads;

	for (i = 0; i < OS_THREAD_MAX_N; ++i, ++slot) {
		if (!slot->in_use) {
			break;
		}
	}

	/* Check if we have run out of slots. */
	if (slot == lock_sys->waiting_threads+ OS_THREAD_MAX_N) {

		ut_print_timestamp(stderr);

		fprintf(stderr,
			"  InnoDB: There appear to be %lu user"
			" threads currently waiting\n"
			"InnoDB: inside InnoDB, which is the"
			" upper limit. Cannot continue operation.\n"
			"InnoDB: We intentionally generate"
			" a seg fault to print a stack trace\n"
			"InnoDB: on Linux. But first we print"
			" a list of waiting threads.\n", (ulong) i);

		lock_wait_table_print();

		ut_error;
	} else {

		ut_a(slot->in_use == FALSE);

		slot->in_use = TRUE;
		slot->thr = thr;
		slot->thr->slot = slot;
		slot->id = os_thread_get_curr_id();
		slot->handle = os_thread_get_curr();

		if (slot->event == NULL) {
			slot->event = os_event_create(NULL);
			ut_a(slot->event);
		}

		os_event_reset(slot->event);
		slot->suspended = TRUE;
		slot->suspend_time = ut_time();
	}

	if (slot == lock_sys->last_slot) {
		++lock_sys->last_slot;
	}

	ut_ad(lock_sys->last_slot
	      <= lock_sys->waiting_threads+ OS_THREAD_MAX_N);

	return(slot);
}

/***************************************************************//**
Puts a user OS thread to wait for a lock to be released. If an error
occurs during the wait trx->error_state associated with thr is
!= DB_SUCCESS when we return. DB_LOCK_WAIT_TIMEOUT and DB_DEADLOCK
are possible errors. DB_DEADLOCK is returned if selective deadlock
resolution chose this transaction as a victim. */
UNIV_INTERN
void
lock_wait_suspend_thread(
/*=====================*/
	que_thr_t*	thr)	/*!< in: query thread associated with the
				user OS thread */
{
	srv_slot_t*	slot;
	double		wait_time;
	trx_t*		trx;
	ulint		had_dict_lock;
	ibool		was_declared_inside_innodb	= FALSE;
	ib_int64_t	start_time			= 0;
	ib_int64_t	finish_time;
	ulint		sec;
	ulint		ms;
	ulong		lock_wait_timeout;

	trx = thr_get_trx(thr);

	lock_wait_mutex_enter();

	trx_mutex_enter(trx);

	trx->error_state = DB_SUCCESS;

	if (thr->state == QUE_THR_RUNNING) {

		ut_ad(thr->is_active == TRUE);

		/* The lock has already been released or this transaction
		was chosen as a deadlock victim: no need to suspend */

		if (trx->lock.was_chosen_as_deadlock_victim) {

			trx->error_state = DB_DEADLOCK;
			trx->lock.was_chosen_as_deadlock_victim = FALSE;
		}

		trx_mutex_exit(trx);

		lock_wait_mutex_exit();
		return;
	}

	ut_ad(thr->is_active == FALSE);

	slot = lock_wait_table_reserve_slot(thr);

	if (thr->lock_state == QUE_THR_LOCK_ROW) {
		srv_n_lock_wait_count++;
		srv_n_lock_wait_current_count++;

		MONITOR_INC(MONITOR_ROW_LOCK_CURRENT_WAIT);

		if (ut_usectime(&sec, &ms) == -1) {
			start_time = -1;
		} else {
			start_time = (ib_int64_t) sec * 1000000 + ms;
		}
	}

	/* Wake the lock timeout monitor thread, if it is suspended */

	os_event_set(srv_timeout_event);

	trx_mutex_exit(trx);
	lock_wait_mutex_exit();

	if (trx->declared_to_be_inside_innodb) {

		was_declared_inside_innodb = TRUE;

		/* We must declare this OS thread to exit InnoDB, since a
		possible other thread holding a lock which this thread waits
		for must be allowed to enter, sooner or later */

		srv_conc_force_exit_innodb(trx);
	}

	had_dict_lock = trx->dict_operation_lock_mode;

	switch (had_dict_lock) {
	case 0:
		break;
	case RW_S_LATCH:
		/* Release foreign key check latch */
		row_mysql_unfreeze_data_dictionary(trx);
		break;
	default:
		/* There should never be a lock wait when the
		dictionary latch is reserved in X mode.  Dictionary
		transactions should only acquire locks on dictionary
		tables, not other tables. All access to dictionary
		tables should be covered by dictionary
		transactions. */
		ut_error;
	}

	ut_a(trx->dict_operation_lock_mode == 0);

	/* Suspend this thread and wait for the event. */

	ut_ad(!trx_mutex_own(trx));
	os_event_wait(slot->event);

	/* After resuming, reacquire the data dictionary latch if
	necessary. */

	if (had_dict_lock) {

		row_mysql_freeze_data_dictionary(trx);
	}

	if (was_declared_inside_innodb) {

		/* Return back inside InnoDB */

		srv_conc_force_enter_innodb(trx);
	}

	wait_time = ut_difftime(ut_time(), slot->suspend_time);

	/* Release the slot for others to use */

	lock_wait_table_release_slot(slot);

	if (thr->lock_state == QUE_THR_LOCK_ROW) {
		ulint	diff_time;

		if (ut_usectime(&sec, &ms) == -1) {
			finish_time = -1;
		} else {
			finish_time = (ib_int64_t) sec * 1000000 + ms;
		}

		diff_time = (ulint) (finish_time - start_time);

		srv_n_lock_wait_current_count--;
		srv_n_lock_wait_time = srv_n_lock_wait_time + diff_time;

		MONITOR_DEC(MONITOR_ROW_LOCK_CURRENT_WAIT);

		if (diff_time > srv_n_lock_max_wait_time &&
		    /* only update the variable if we successfully
		    retrieved the start and finish times. See Bug#36819. */
		    start_time != -1 && finish_time != -1) {
			srv_n_lock_max_wait_time = diff_time;
		}
	}

	/* InnoDB system transactions (such as the purge, and
	incomplete transactions that are being rolled back after crash
	recovery) will use the global value of
	innodb_lock_wait_timeout, because trx->mysql_thd == NULL. */
	lock_wait_timeout = trx_lock_wait_timeout_get(trx);

	if (lock_wait_timeout < 100000000
	    && wait_time > (double) lock_wait_timeout) {

		trx->error_state = DB_LOCK_WAIT_TIMEOUT;
	}

	if (trx_is_interrupted(trx)) {

		trx->error_state = DB_INTERRUPTED;
	}
}

/********************************************************************//**
Releases a user OS thread waiting for a lock to be released, if the
thread is already suspended. */
UNIV_INTERN
void
lock_wait_release_thread_if_suspended(
/*==================================*/
	que_thr_t*	thr)	/*!< in: query thread associated with the
				user OS thread	 */
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(thr_get_trx(thr)));

	/* We own both the lock mutex and the trx_t::mutex but not the
	lock wait mutex. This is OK because other threads will see the state
       	of this mutex as being in use and no other thread can change the state
       	of the slot to free unless that thread also owns the lock mutex. */

	if (thr->slot != NULL && thr->slot->in_use && thr->slot->thr == thr) {
		trx_t*	trx = thr_get_trx(thr);

		if (trx->lock.was_chosen_as_deadlock_victim) {

			trx->error_state = DB_DEADLOCK;
			trx->lock.was_chosen_as_deadlock_victim = FALSE;
		}

		MONITOR_INC(MONITOR_TIMEOUT);

		os_event_set(thr->slot->event);
	}
}

/*********************************************************************//**
Check if the thread lock wait has timed out. Release its locks if the
wait has actually timed out. */
static
void
lock_wait_check_and_cancel(
/*=======================*/
	const srv_slot_t*	slot)	/*!< in: slot reserved by a user
				       	thread when the wait started */
{
	trx_t*		trx;
	double		wait_time;
	ulong		lock_wait_timeout;
	ib_time_t	suspend_time = slot->suspend_time;

	ut_ad(lock_wait_mutex_own());

	wait_time = ut_difftime(ut_time(), suspend_time);

	trx = thr_get_trx(slot->thr);

	lock_mutex_enter();

	trx_mutex_enter(trx);

	lock_wait_timeout = trx_lock_wait_timeout_get(trx);

	if (trx_is_interrupted(trx)
	    || (lock_wait_timeout < 100000000
		&& (wait_time > (double) lock_wait_timeout
		   || wait_time < 0))) {

		/* Timeout exceeded or a wrap-around in system
		time counter: cancel the lock request queued
		by the transaction and release possible
		other transactions waiting behind; it is
		possible that the lock has already been
		granted: in that case do nothing */

		if (trx->lock.wait_lock) {

			ut_a(trx->lock.que_state == TRX_QUE_LOCK_WAIT);

			lock_cancel_waiting_and_release(trx->lock.wait_lock);
		}
	}

	trx_mutex_exit(trx);
	
	lock_mutex_exit();
}

/*********************************************************************//**
A thread which wakes up threads whose lock wait may have lasted too long.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
lock_wait_timeout_thread(
/*=====================*/
	void*	arg __attribute__((unused)))
			/* in: a dummy parameter required by
			os_thread_create */
{
	ib_int64_t	sig_count = 0;

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_lock_timeout_thread_key);
#endif

	do {
		srv_slot_t*	slot;

		server_mutex_enter();

		srv_lock_timeout_active = FALSE;

		server_mutex_exit();

		/* When someone is waiting for a lock, we wake up every second
		and check if a timeout has passed for a lock wait */

		os_event_wait_time_low(srv_timeout_event, 1000000, sig_count);

		/* Note: In logs_empty_and_mark_files_at_shutdown() we
		check the srv_lock_timeout_active flag to determine whether
		this thread has exited or not. There is a small window here
		where the flag can be FALSE before we set it to TRUE below.
		This is OK because:

		  1. We will exit after executing the code
		  2. logs_empty_and_mark_files_at_shutdown() has additional
		     checks that will block the shutdown from proceeding
		     to the next state. */

		if (srv_shutdown_state >= SRV_SHUTDOWN_CLEANUP) {
			break;
		}

		server_mutex_enter();

		srv_lock_timeout_active = TRUE;

		server_mutex_exit();

		lock_wait_mutex_enter();

		/* Check all slots for user threads that are waiting
	       	on locks, and if they have exceeded the time limit. */

		for (slot = lock_sys->waiting_threads;
		     slot < lock_sys->last_slot;
		     ++slot) {

			/* We are doing a read without the lock mutex
			and/or the trx mutex. This is OK because a slot
		       	can't be freed or reserved without the lock wait
		       	mutex. */

			if (slot->in_use) {
				lock_wait_check_and_cancel(slot);
			}
		}

		sig_count = os_event_reset(srv_timeout_event);

		lock_wait_mutex_exit();

	} while (srv_shutdown_state < SRV_SHUTDOWN_CLEANUP);

	server_mutex_enter();

	srv_lock_timeout_active = FALSE;

	server_mutex_exit();

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

