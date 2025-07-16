#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
	 Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

/* H/W Clock */
/* Sets up the 8254 Programmable Interval Timer (PIT) to
	 interrupt PIT_FREQ times per second, and registers the
	 corresponding interrupt. */
/* */
void timer_init(void)
{
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
		 nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb(0x43, 0x34); /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb(0x40, count & 0xff);
	outb(0x40, count >> 8);

	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
	// 특정 시간 마다 한 번씩 커널에 타이머
	// 인터럽트가 들어오도록 PIT를 구성.
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
		 still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks(void)
{
	enum intr_level old_level = intr_disable();
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
	 should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void timer_sleep(int64_t ticks)
{
	int64_t start = timer_ticks(); // 부팅 이후 tick 수

	ASSERT(intr_get_level() == INTR_ON);
	// while (timer_elapsed(start) < ticks)
	// 	thread_yield(); // CPU를 양보한다. // ready-list에 추가된다.
	// yield the cpu, insert to ready-list

	if (timer_elapsed(start) < ticks) // 매개변수 ticks
	{
		thread_sleep(start + ticks); // 미래의 절대 시점을 의미하게 된다.
	}
}

/* Suspends execution for approximately MS milliseconds. */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

// /* Timer interrupt handler. */ /*처음 시도*/
// static void
// timer_interrupt(struct intr_frame *args UNUSED)
// {
// 	ticks++; // tick을 계속 늘려가면서 기록한다.
// 	thread_tick();

// 	/* check sleep list and the global tick, find any threads to wake up */
// 	/* 삭제 전 다음 노드 저장 */
// 	struct thread *curr = list_begin(get_sleep_list());
// 	while (curr->wakeup_tick > timer_ticks())
// 	{
// 		if (curr->wakeup_tick <= timer_ticks())
// 		{
// 			break;
// 		}
// 		curr = list_next(curr);
// 	}

// 	/* move them to the ready list, if necessary update the global tick*/ // 왜 global tick을 필요시에 업데이트하라고 하는거지?
// 	curr->status = THREAD_READY;
// 	list_push_back(get_ready_list(), curr);
// }

/* Timer interrupt handler. */ /* 시도 2 */
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++; // tick을 계속 늘려가면서 기록한다.
	thread_tick();

	/* check sleep list and the global tick, find any threads to wake up */
	enum intr_level old_level;
	old_level = intr_disable();

	struct list_elem *node;
	struct list *sleep_list = get_sleep_list();
	struct thread *curr;
	// 리스트 삭제 시 위험 -> 리스트 정렬해서 리스트의 헤드만 확인하는 방향으로 디벨롭
	while (!list_empty(sleep_list))
	{
		node = list_front(sleep_list);
		curr = list_entry(node, struct thread, elem);
		if ((curr->wakeup_tick) > ticks)
		// wakeup_tick은 미래의 시점이기 때문에 모두 ticks보다 크다.
		// curr->wakeup_tick >ticks인 경우는 아직 깨울 시간이 아니라는 의미.
		{
			break;
		}
		curr->status = THREAD_READY;
		// priority 역순으로 정렬 ?
		// 정해진 시간을 지나면 thread를 깨운다. ready-list에 넣을 때는 우선순위로 정렬해서 집어넣는다.
		list_insert_ordered(get_ready_list(), list_pop_front(sleep_list), dsc_pri, NULL);
	}
	intr_set_level(old_level);

	// while (e != list_end(get_sleep_list()))
	// {
	// 	struct thread *curr = list_entry(e, struct thread, elem);
	// 	// if (curr->wakeup_tick >= timer_ticks()) // 부등호 확인
	// 	next = list_next(e); // 삭제 전에 next를 저장해준다.

	// 	// priority로 접근하는 건 잘못된 접근 방법임.
	// 	// if (curr->priority > thread_current()->priority)
	// 	// {
	// 	// 	thread_yield();
	// 	// 	list_remove(e);
	// 	// 	thread_unblock(curr);
	// 	// }

	// 	// list_push_back 스레드를 뒤에서 부터 저장
	// 	// if (curr->wakeup_tick <= timer_ticks())
	// 	// {
	// 	// 	// curr->status = THREAD_READY;
	// 	// 	// list_push_back(get_ready_list(), e);
	// 	// 	list_remove(e);
	// 	// 	thread_unblock(curr); // 스레드를 깨운다. // add to ready list
	// 	// }
	// e = next;
	// }
	/* move them to the ready list, if necessary update the global tick*/ // 왜 global tick을 필요시에 업데이트하라고 하는거지?
}

/* Returns true if LOOPS iterations waits for more than one timer
	 tick, otherwise false. */
static bool
too_many_loops(unsigned loops)
{
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	barrier();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
	 brief delays.

	 Marked NO_INLINE because code alignment can significantly
	 affect timings, so that if this function was inlined
	 differently in different places the results would be difficult
	 to predict. */
static void NO_INLINE
busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep(int64_t num, int32_t denom)
{
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

		 (NUM / DENOM) s
		 ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
		 1 s / TIMER_FREQ ticks
		 */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* We're waiting for at least one full timer tick.  Use
			 timer_sleep() because it will yield the CPU to other
			 processes. */
		timer_sleep(ticks);
	}
	else
	{
		/* Otherwise, use a busy-wait loop for more accurate
			 sub-tick timing.  We scale the numerator and denominator
			 down by 1000 to avoid the possibility of overflow. */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
