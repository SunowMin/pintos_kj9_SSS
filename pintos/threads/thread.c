#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

#define CALC_F (1 << 14)
#define FLOAT(n) ((n) * (CALC_F))
#define INT(x) ((x) / (CALC_F))
#define MUL_FLOATS(x, y) (((int64_t)(x)) * (y) / CALC_F)
#define DIV_FLOATS(x, y) (((int64_t)(x)) * CALC_F / (y))

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running.
   질리도록 언급된 ready_list, ready_queue. 맨 앞 걸 꺼내서 실행하는 식. */
static struct list ready_list;

/* Alarm clock timer_sleep 구현을 위함. */
static struct list sleep_list;

/* mlfqs에서만 사용. 모든 쓰레드를 관리. blocked까지. */
static struct list all_list;

/* Idle thread. */
struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */


/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs".
   그니까 일단은 ROUND ROBIN을 써야 한단 얘기다!!! */
bool thread_mlfqs;

// [구현 4-0] `load_avg` 전역변수 추가
static int load_avg = 0;

/* 각 쓰레드의 틱값 중 최솟값 */
static int64_t min_wakeup_ticks = INT64_MAX;     

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
static tid_t allocate_tid (void);



/* Returns true if T appears to point to a valid thread. 보통 ASSERT 용. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);

	/* 모든 쓰레드를 관리할 리스트: mlfqs에선 필요*/
	if(thread_mlfqs){
		list_init(&all_list);
	}

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();

	if(thread_mlfqs){
		list_push_back(&all_list, &(initial_thread -> d_elem));
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. 
   
   일단 이건 바꿔야 한다는 소리임. Priority 스케줄링 필요함!  */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	enum intr_level old_level;
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();
	
	

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;


	old_level = intr_disable ();
	if(thread_mlfqs){
		list_push_back(&all_list, &(t->d_elem)); 
	}
	intr_set_level (old_level);

	/* Add to run queue. */
	thread_unblock (t);

	// [구현 2-2] 현재 쓰레드보다, 새롭게 추가된 쓰레드의 priority가 높은 경우 yield할 것.
	check_front_yield();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);

// 	for(struct list_elem *node = list_begin(&ready_list); node != list_end(&ready_list); node = list_next(node)){
//     printf("lock 주소 %d\n", list_entry(node, struct thread, d_elem) -> priority);
//   }

	// [구현 2-1B] Priority 내림차순을 지킬 수 있게, 올바른 위치에 삽입할 것.
	//list_push_back (&ready_list, &t->elem);
	list_insert_ordered(&ready_list, &t->elem, dsc_priority, NULL);
	t->status = THREAD_READY;
	// }

	
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	if(thread_mlfqs){
		list_remove(&(thread_current()->d_elem)); 
	}
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. 
   현재 쓰레드 노드를 ready_list에 삽입하고, (THREAED_RUNNING) -> (THREAD_READY)로 수정한 뒤
   다음 쓰레드 스케줄링 실행. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	// struct thread *next = next_thread_to_run ();	// 놀랍게도 이게 문제였음
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	// if (next -> priority < curr -> priority){
	// 	intr_set_level (old_level);
	// 	return;
	// }
	if (curr != idle_thread)
		// [구현 2-1A] Priority 내림차순을 지킬 수 있게, 올바른 위치에 삽입할 것.
		list_insert_ordered(&ready_list, &curr->elem, dsc_priority, NULL);
		// list_push_back (&ready_list, &curr->elem);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	// [구현 2-3] 우선순위를 재설정. 현재 쓰레드의 우선순위가 더이상 highest가 아닌 경우 yield.
	// [구현 3-5] donation 고려하여, 새롭게 set한 priority와 현재 donate된 priority 중 높은 쪽을 priority로 설정
	// [구현 4-2] mlfqs 사용 시 비활성화
	if (thread_mlfqs)
		return;
	thread_current () -> saved_priority = new_priority;
	thread_current () -> priority = new_priority;
	
	// donor list 재정렬 후, priority 갱신
	if (!list_empty(&(thread_current() -> donations))){
		list_sort(&(thread_current() -> donations), dsc_donor_priority, NULL);
		donate(thread_current());
	}
	
	// ready list 재정렬
	//list_sort(&ready_list, dsc_priority, NULL);
	check_front_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	// [구현 3-6] priority를 기부받은 경우, donated priority를 반환해야 함
	// 그런데 우리는 saved_priority를 따로 두고 있고, 원래 priority는 그대로니, 더 뭘 해줄 게 없음.
	return thread_current ()->priority;
}

// [구현 4-5] 각종 SET/GET 함수.
/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	// [구현 4-5A] 현재 쓰레드의 nice를 재설정.
	thread_current() -> nice = nice;
	calc_priority(thread_current());
	list_sort(&ready_list, dsc_priority, NULL);
	check_front_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	// [구현 4-5B] 현재 쓰레드의 nice를 반환.
	return thread_current() -> nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	// [구현 4-5C] load_avg * 100을 반환
	// load_avg는 fixed point... 인데 integer로 다시 바꿔야 하나
	//printf("%d\n", load_avg);
	// printf("%d - %d\n", load_avg, INT(load_avg * 100));
	return INT(load_avg * 100);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	// [구현 4-5D] 현재 쓰레드의 recent_cpu * 100을 반환
	// recent_cpu는 fixed_point, 100은 integer.
	//printf("%d - %d\n", thread_current() -> recent_cpu, INT(thread_current() -> recent_cpu * 100));
	return INT(thread_current() -> recent_cpu * 100);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
// [구현 3-2] donations 리스트 설정하기.
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	
	t->priority = priority;

	if(thread_mlfqs){
		t->nice = 0;
		t->recent_cpu= 0;
	} else {
		t->wait_on_lock = NULL;
		t->saved_priority = priority;
		list_init(&t -> donations);
	}
	
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. 
   그러니까 ready_list에서 맨 앞 노드를 뽑아오고, 이를 struct thread로 바꿔 준다는 소립니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. 
   
   !!!이해하려 하지 않아도 됨!!!  */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	// THREAD_DYING으로 된 애들은 다음 쓰레드로 전환 후 모두 사라진다. 지금 그걸 해 주는 거임.
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}

	// 현재 쓰레드의 status를 매개변수로 바꾼다.
	thread_current ()->status = status;

	// 새롭게 스케줄링
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	// interrupt가 disabled 상태인가?
	ASSERT (intr_get_level () == INTR_OFF);

	// 현재 쓰레드가 running 상태가 아닌가?
	ASSERT (curr->status != THREAD_RUNNING);

	// next 쓰레드가 실제 valid한 쓰레드인가?
	ASSERT (is_thread (next));

	/* Mark status as running. */
	// 다음 쓰레드를 실행한단 소리지
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	// 당연히 똑같은 노드면 이렇게 해 줄 필요가 없겠지
	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule().
		   dying: The thread will be destroyed by the scheduler after switching to the next thread.
		   그러니까 THREAD_DYING 상태면 리스트 맨 뒤에 push하면 됨. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

/* tick의 오름차순으로 정렬할 시 사용되는 함수. */
bool asc_ticks (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct thread *tx = list_entry(x, struct thread, elem);
	struct thread *ty = list_entry(y, struct thread, elem);
	return (tx -> wakeup_tick) < (ty -> wakeup_tick);
}

/* priority의 내림차순으로 정렬할 시 사용되는 함수. */
bool dsc_priority (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct thread *tx = list_entry(x, struct thread, elem);
	struct thread *ty = list_entry(y, struct thread, elem);
	return (tx -> priority) > (ty -> priority);
}

// function that sets thread state to sleep
void thread_sleep (int64_t ticks){
	// 함수를 호출한 쓰레드 자기 자신
	struct thread *curr = thread_current();

	// interrupt 차단하기
	enum intr_level old_level;
	old_level = intr_disable();

	if (curr != idle_thread){

		// 현재 쓰레드의 local wakeup_tick 저장.
		curr -> wakeup_tick = ticks;

		// 전역 min_wakeup_ticks 변수를 수정
		if (get_min_ticks() > ticks){
			save_min_ticks(ticks);
		}

		// 슬립 큐에 넣기 (슬립큐는 깨울 틱 수의 오름차순)
		list_insert_ordered(&sleep_list, &curr->elem, asc_ticks, NULL);
		// printf("(%s) 슬립 큐에 넣었습니다\n", curr -> name);

		// printf("ticks: %lld, global ticks: %lld\n", curr -> wakeup_tick, get_min_ticks());

		// 지금 쓰레드는 BLOCKED로 전환 -> 스케줄링.
		do_schedule(THREAD_BLOCKED);
		
	}
	intr_set_level(old_level);
};

void wake_up(int64_t ticks){
	// interrupt 차단하기
	enum intr_level old_level;
	old_level = intr_disable();

	// 깨울 쓰레드가 있는지 확인하기
	if (ticks >= min_wakeup_ticks){
		// printf("ticks: %lld, 깨울 쓰레드를 찾았습니다\n", ticks);

		struct list_elem *node;
		struct thread *curr_thread;

		// sleep 리스트의 머리를 확인. ticks가 작은 순서부터. 깨울 thread가 있으면 찾음.
		while (!list_empty(&sleep_list)){
			node = list_front(&sleep_list);
			curr_thread = list_entry(node, struct thread, elem);

			// 더이상 깨울 쓰레드가 없음
			if ((curr_thread -> wakeup_tick) > ticks){
				
				break;
			}
			// 레디 큐에 넣기 (레디 큐는 priority의 내림차순)
			curr_thread -> status = THREAD_READY;
			list_insert_ordered(&ready_list, list_pop_front(&sleep_list), dsc_priority, NULL);
			// printf("노드 %lld를 깨웠습니다\n", curr_thread -> wakeup_tick);
		}

		// printf("모든 쓰레드를 깨웠습니다\n");
		// global tick을 update
		if (list_empty(&sleep_list)){
			save_min_ticks(INT64_MAX);
		} else {
			save_min_ticks(curr_thread -> wakeup_tick);
		}
	}
	intr_set_level(old_level);
}

// Saves the minimum value of ticks that threads have
void save_min_ticks(int64_t new_value){
	min_wakeup_ticks = new_value;
}

// Returns the minimum value of ticks
int64_t get_min_ticks(void){
	return min_wakeup_ticks;
}

// 현재 대기 중인 priority 최댓값 thread가
// 현재 thread의 priority보다 높은 경우 yield함
void check_front_yield(void){
	if(!list_empty(&ready_list) && list_entry(list_front(&ready_list), struct thread, elem) -> priority > (thread_get_priority())){
		thread_yield();
	}
}

// [구현 4-4] priority 계산 위한 각종 함수들

void recent_cpu_up_one(void){
	// `recent_cpu`를 1 증가시키는 함수. 
	// recent_cpu는 fixed-point.
	if(thread_mlfqs){
		(thread_current() -> recent_cpu) += 1 * (CALC_F);
	}
}

void calc_load_avg(void){
	// `load_avg`를 재계산하는 함수.
	// load_avg는 fixed point, ready_threads는 integer
	if(thread_mlfqs){
		int ready_threads = list_size(&ready_list);
		if (thread_current() != idle_thread){
			ready_threads += 1;
		}
		//printf("ready_threads %d, load_avg %d\n", ready_threads, load_avg);
		load_avg = (MUL_FLOATS(DIV_FLOATS(FLOAT(59), FLOAT(60)), load_avg) + DIV_FLOATS(FLOAT(1), FLOAT(60)) * ready_threads);
		
	}
}

void calc_recent_cpu(struct thread *t){
	if(thread_mlfqs){
	// load_avg, recent_cpu, decay도 fixed point. nice는 integer.
	int decay = DIV_FLOATS(2 * load_avg, 2 * load_avg + CALC_F);
	int result = MUL_FLOATS(decay, t -> recent_cpu) + FLOAT(t -> nice);
	t -> recent_cpu = result;
	}
}

void calc_priority(struct thread *t){
	if(thread_mlfqs){
	// priority, nice는 integer. recent_cpu는 fixed point.
	t -> priority = INT(FLOAT(PRI_MAX) - (t -> recent_cpu / 4) - FLOAT(t -> nice * 2));
	}
}

void calc_all_recent_cpu(void){
	if(thread_mlfqs){
	for (struct list_elem *node = list_begin(&all_list); node != list_end(&all_list); node = list_next(node)){
		calc_recent_cpu(list_entry(node, struct thread, d_elem));
	};
	}
}

void calc_all_priority(void){
	if(thread_mlfqs){
	calc_priority(thread_current());
	for (struct list_elem *node = list_begin(&all_list); node != list_end(&all_list); node = list_next(node)){
		calc_priority(list_entry(node, struct thread, d_elem));
	};
}

}