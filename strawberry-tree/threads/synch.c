/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"



/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	// [구현 2-4] Priority 내림차순으로, 올바른 위치에 삽입
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		// priority 기준으로 올바른 위치에 삽입해 주는 코드
		list_insert_ordered(&sema->waiters, &thread_current () -> elem, dsc_priority, NULL);
		thread_block();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	// [구현 2-5] 깨운 쓰레드의 priority가 더 높은 경우, CPU는 yield해줘야 함
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	
	if (!list_empty (&sema->waiters)){
		list_sort(&sema -> waiters, dsc_priority, NULL);
// 		for(struct list_elem *node = list_begin(&sema->waiters); node != list_end(&sema->waiters); node = list_next(node)){
//     printf("priority %d\n", list_entry(node, struct thread, elem) -> priority);
//   }
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	}
	sema->value++;

	// 깨운 쓰레드의 priority가 더 높은 경우 yield
	check_front_yield();
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	// [구현 3-3] lock을 이미 얻은 thread가 있는 경우, 이에 맞게 구조체 변경 필요
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));
	struct thread *lock_holder = lock -> holder;

	// lock을 이미 얻은 thread가 있는 경우
	// [구현 4-3A] mlfqs 사용 시 `lock_acquire`, `lock_release`의 priority donation 비활성화
	if (lock_holder != NULL && !thread_mlfqs){
		
		// (1) 현재 쓰레드의 `wait_on_lock`을 갱신
		thread_current() -> wait_on_lock = lock;

		// (2) lock을 가진 쓰레드의 donations에, 현재 쓰레드를 추가. priority의 내림차순으로
		list_insert_ordered(&(lock_holder -> donations), &(thread_current() -> d_elem), dsc_donor_priority, NULL);

		// printf("삽입 결과: %p, NEXT: %p\n", thread_current() -> d_elem, list_next(&(thread_current() -> d_elem)));

		// (3) 현재 lock을 가진 쓰레드의 priority를, donations 맨 앞 위치한 노드의 priority로 갱신. 단, 갱신할 priority가 높을 때만.
		donate(lock_holder);
	}	

	sema_down (&lock->semaphore);
	lock->holder = thread_current ();
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	// [구현 3-4] priority donation 구현
	// [구현 4-3A] mlfqs 사용 시 `lock_acquire`, `lock_release`의 priority donation 비활성화
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	
	if (!thread_mlfqs){
		enum intr_level old_level;
		old_level = intr_disable ();

		// (1) lock -> holder의 donations 리스트를 순회한다.
		// 현재 lock과 wait_on_lock 멤버가 일치하면
		// 해당 노드를 삭제하고, wait_on_lock 멤버도 NULL로 바꾼다.
		struct list *donors = &(thread_current() -> donations);
		struct list_elem *node;
		struct thread *curr_thread; 
		int found_flag = 0;

		if (!list_empty(donors)){
			struct list_elem *next_node;
			//printf("리스트 크기: %d\n", list_size(donors));
			node = list_begin(donors);

			while (node != list_end(donors)){
				curr_thread = list_entry(node, struct thread, d_elem);
				if (lock == curr_thread -> wait_on_lock){
					found_flag = 1;
					curr_thread -> wait_on_lock = NULL;
					next_node = list_next(node);
					list_remove(node);
					node = next_node;
				}
				else {
					node = list_next(node);
				}
			}
		}
		
		// (2) priority를 남은 `donations` 리스트 중 최댓값 priority로 재설정한다. 단 리스트가 빈 경우, `saved_priority`로 재설정한다.
		if (found_flag){
		thread_current() -> priority = thread_current() -> saved_priority;
		if (!list_empty(&(thread_current() -> donations))){
			donate(thread_current());
		}}

		intr_set_level(old_level);
	}
	

	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
/* 이걸 바꿔도 되나 아님 priority 확인할 다른 방법이 있나...*/
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
	struct thread *thread;				// 정렬 용도 priority.
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	// [구현 2-6] Priority 내림차순으로, 올바른 위치에 삽입

	// cond_signal이 해당 condition으로 시그널을 보낼 때까지 대기해야 한다.
	// waiters 리스트에 대기할 waiter는 struct semaphore_elem 형으로 정의
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));
	sema_init (&waiter.semaphore, 0);
	waiter.thread = thread_current();

    // cond의 waiters 리스트(&cond -> waiters)에 지금 쓰레드 (&waiter.elem)를 넣는다. priority의 역순으로 넣는다.
	list_insert_ordered (&cond->waiters, &waiter.elem, dsc_sema_priority, NULL);
	lock_release (lock);  // 기존 모니터 락이 임시 해제
	sema_down (&waiter.semaphore);  // signal 올때까지 wait
	lock_acquire (lock);  // wait이 풀림
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

/* waiters 리스트 -> priority의 내림차순으로 정렬할 시 사용되는 함수. */
bool dsc_sema_priority (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct semaphore_elem *tx = list_entry(x, struct semaphore_elem, elem);
	struct semaphore_elem *ty = list_entry(y, struct semaphore_elem, elem);
	return (tx -> thread -> priority) > (ty -> thread -> priority);
}

/* priority의 내림차순으로 정렬할 시 사용되는 함수. */
bool dsc_donor_priority (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct thread *tx = list_entry(x, struct thread, d_elem);
	struct thread *ty = list_entry(y, struct thread, d_elem);
	return (tx -> priority) > (ty -> priority);
}

// 입력받은 쓰레드의 priority와, donations 리스트 맨 앞의 priority를 비교 후, 기부가 필요하면 기부.
void donate(struct thread *c_thread){
	struct list *c_donations = &(c_thread -> donations);
	int front_priority = list_entry(list_begin(c_donations), struct thread, d_elem) -> priority;
	
	// 현재 lock을 가진 쓰레드의 priority를, donations 맨 앞 위치한 노드의 priority로 갱신
	// 단, 갱신할 priority가 높은 경우만
	if ((c_thread -> priority) < front_priority){
		c_thread -> priority = front_priority;
		
	}

	// [구현 3-7] Nested Donation 구현
	if (c_thread -> wait_on_lock != NULL){
		donate(c_thread -> wait_on_lock -> holder);
	}
}
