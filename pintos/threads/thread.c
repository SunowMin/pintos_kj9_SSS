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
#include "devices/timer.h"
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

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Alarm clock을 위해 리스트 선언, timer_sleep으로 블로킹한 스레드를 관리 */
static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests 스레드 파괴 요청
	스레드가 완전히 종료되기 전에 소멸처리를 요청하는 스레드들을 모아두는 리스트
*/
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
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* 스레드의 틱 값 중 최소값 */
static int64_t min_wakeup_ticks = INT64_MAX;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

void save_min_ticks(int64_t new_value);
int64_t get_min_ticks(void);
void thread_wakeup(int64_t ticks);
void thread_sleep(int64_t ticks);
static bool dsc_priority (const struct list_elem *x, const struct list_elem *y, const void *aux);


/* Returns true if T appears to point to a valid thread. */
// 매직 넘버 확인하는 부분 
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
   finishes. 
   
 	현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
	이 방법은 일반적으로는 작동하지 않으며, 이 경우에는 loader.S가 스택 하단을 페이지 경계에 배치했기 때문에 가능합니다.
	실행 큐와 tid 잠금도 초기화합니다.
	이 함수를 호출한 후에는 thread_create()를 사용하여 스레드를 생성하기 전에 페이지 할당자를 초기화해야 합니다.
	이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다.
*/
void
thread_init (void) {
	// 인터럽트가 꺼져 있는지 확인
	// 초기화 중에 인터럽트가 들어오면 자료구조가 꼬일 수 있으니 꺼둔 상태에서 초기화해야함
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init ().
	 * 커널의 임시 GDT를 다시 로드합니다.
	* 이 GDT에는 사용자 컨텍스트가 포함되지 않습니다.
	* 커널은 gdt_init()에서 사용자 컨텍스트를 사용하여 GDT를 다시 빌드합니다.
	(GDT : 글로벌 디스크립터 테이블)
	*/
// 일단 커널모드에서 쓸 GDT를 CPU 레지스터에 로드
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	// 동시에 두 스레드가 접근하는걸 막기위해 락을 걸어둔 용도
	lock_init (&tid_lock);
	// 준비 상태인 스레드가 줄 서있는 큐
	list_init (&ready_list);
	// 종료된 스레드 모아놓는 리스트
	list_init (&destruction_req);
	// sleep_list 초기화
	list_init (&sleep_list);

	/* Set up a thread structure for the running thread. 
	실행 중인 스레드에 대한 스레드 구조를 설정합니다.
	*/
	initial_thread = running_thread ();
	// 구조체 이름:main, 우선순위:PRI_DEFAULT(기본값)
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. 
   인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
	또한 유휴 스레드를 생성합니다.*/
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	// 호출 가능 상태로 만듦
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context.
   타이머 인터럽트 핸들러가 매 타이머 틱마다 호출합니다.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다.
 */
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

   주어진 초기 PRIORITY를 갖는 NAME이라는 이름의 새 커널 스레드를 생성합니다. 
   이 스레드는 AUX를 인수로 전달하는 FUNCTION을 실행하고, 준비 완료 큐에 추가합니다. 
   새 스레드의 스레드 식별자를 반환하며, 생성에 실패하면 TID_ERROR를 반환합니다.

	thread_start()가 호출된 경우, 새 스레드는
	thread_create()가 반환되기 전에 스케줄링될 수 있습니다. 심지어
	thread_create()가 반환되기 전에 종료될 수도 있습니다. 반대로, 원래
	스레드는 새 스레드가 스케줄링되기 전까지 얼마든지 실행될 수 있습니다.
	순서를 보장해야 하는 경우 세마포어 또는 다른 형태의
	동기화를 사용하십시오.

	제공된 코드는 새 스레드의 `priority` 멤버를
	PRIORITY로 설정하지만, 실제 우선순위 스케줄링은 구현되지 않습니다.
	우선순위 스케줄링은 문제 1-3의 목표입니다.
*/
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	// 페이지 크기만큼 말록 하는 역할
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

	/* Add to run queue. */
	thread_unblock (t);

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().
   현재 스레드를 sleep으로 만든다. 
   thread_unblock으로 깨우기 전까지 다시 스케줄되지 않는다.

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. 
   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 한다
   보통은 synch.h에 있는 동기화 프리미티브를 사용하는 것이 더 좋다
*/
void
thread_block (void) {
	// 외부 인터럽트 처리중이 아니어야 함
	ASSERT (!intr_context ());
	// 인터럽트가 비활성화 되어 있어야 함
	ASSERT (intr_get_level () == INTR_OFF);
	// 현재 스레드를 BLOCKED 상태로 바꿈
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)
   blocked상태의 스레드 T를 실행준비상태로 전환한다.
   T가 blocked상태가 아니면 에러 발생
   (실행 중인 스레드를 ready상태로 만들고 싶다면 thread_yield()를 사용하라.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. 
   이 함수는 현재 실행 중인 스레드를 선점(preempt)하지 않는다
   이 점이 중요한 이유는 호출자가 인터럽트를 직접 비활성화한 경우
   스레드를 원자적으로(uninterruptibly=동기적) 언블록하고 다른 데이터도 안전하게 업데이트할 수 있기를 기대할 수 있기 때문이다.*/
// 인터럽트 비활성화하고, T스레드를 ready_list에 추가하고 상태를 READY로 변경, 인터럽트 상태를 원래대로 복원   
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	// t가 유효한 포인터인지 검사 (스레드 구조체가 깨졌거나 잘못된 값은 아닌지 확인)
	ASSERT (is_thread (t));

	// 인터럽트 끄고 동기적(작업위해 다른 방해 멈추게 해서 안전하게 처리하는 것)으로 처리
	old_level = intr_disable ();
	// t의 현재 상태가 BLOCKED인지 확인
	ASSERT (t->status == THREAD_BLOCKED);

	// T스레드를 ready_list의 맨 뒤에 추가
	// list_push_back (&ready_list, &t->elem);
	// ready_list의 맨 뒤가 아닌 Priority 내림차순을 지키면서 삽입
	list_insert_ordered(&ready_list, &t->elem, dsc_priority, NULL);
	
	// 스레드를 READY상태로 변경
	t->status = THREAD_READY;
	// 인터럽트 상태를 원래대로 복원
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. 
   현재 실행 중인 스레드를 반환
   running_thread()에 몇 가지 안정성 검사를 추가한 버전
   자세한 내용은 thread.h 파일 맨 위의 큰 주석을 참조
   */
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
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. 
   CPU를 양보. 현재 스레드는 sleep되지 않으며,
   스케줄러의 결정에 따라 바로 다시 스케줄될 수도 있음
*/
void
thread_yield (void) {
	// 현재 스레드 저장
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	// 지금 커널이 외부 인터럽트 처리 루틴 안에서 돌아가고 있으면 종료
	// 인터럽트 핸들러 코드가 실행 중인 상태면 종료
	ASSERT (!intr_context ());

	// 인터럽트 상태를 저장
	old_level = intr_disable ();
	
	// 현재 스레드가 idle_thread가 아니면 
	if (curr != idle_thread)
		// curr스레드를 ready_list의 맨 뒤에 추가
		// list_push_back (&ready_list, &curr->elem);
		// ready_list의 맨 뒤가 아닌 Priority 내림차순을 지키면서 삽입
		list_insert_ordered(&ready_list, &curr->elem, dsc_priority, NULL);
	// 지금 쓰레드를 READY 상태로 전환 & 스케줄러를 호출해 CPU를 다른 스레드로 넘김
	do_schedule (THREAD_READY);
	// 이전의 인터럽트 상태로 복원
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
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

	// 무한루프 
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
   NAME.
   스레드t를 blocked 상태로 초기화한다 */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	// 스레드 구조체 메모리 전부를 0으로 초기화 (쓰레기값대비)
	memset (t, 0, sizeof *t);
	// 바로 blocked상태로 세팅
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	// 스택 포인터 초기화 t:스레드구조체
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
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

스케줄링 컨텍스트 스위치의 구체적인 메커니즘은 thread/thread.c의 thread_launch() 함수 안에 있습니다(이 부분을 깊게 이해할 필요는 없습니다).
   */
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
 * It's not safe to call printf() in the schedule(). 
 * 새로운 프로세스를 스케줄링한다. 진입 시에는 인터럽트가 꺼져 있어야 한다
 * 이 함수는 현재 스레드의 상태를 주어진 status로 변경한 뒤,
 * 실행할 다른 스레드를 찾아서 그 스레드로 전환한다.
 * schedule() 함수 안에서는 printf()를 호출하는 것이 안전하지 않다.
 * */
// 현재 스레드를 내가 원하는 상태로 전환한 뒤 CPU를 양보하고 빠지기
static void
do_schedule(int status) {
	// 인터럽트 비활성화 상태여야 함
	ASSERT (intr_get_level () == INTR_OFF);
	// 현재 스레드의 상태가 RUNNING이어야 함
	ASSERT (thread_current()->status == THREAD_RUNNING);
	// 종료해야 할 요소들을 모아놓은 리스트가 비어있지 않다면 
	while (!list_empty (&destruction_req)) {
		// destruction_req리스트에 모아뒀던 종료대기스레드들을 하나씩 꺼내서 victim변수에 저장
		struct thread *victim =
		// destruction_req리스트 맨 앞에 있는 list_elem포인터를 꺼내서 struct thread 타입으로 변환
		// 스레드 전체 정보에 접근하기 위함
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		// 각 스레드가 쓰던 페이지를 메모리 할당자에게 반환
		palloc_free_page(victim);
	}
	// 현재 실행 중인 스레드의 상태를 인자로 전달받은 status로 바꾸고
	thread_current ()->status = status;
	// 스케줄러를 불러서 다음 스레드로 교체
	schedule ();
}

// 현재 스레드를 내리고 ready_list에서 다음 스레드를 선택해 컨텍스트 스위치를 준비하는 함수
static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	// next가 유효한지 검사
	ASSERT (is_thread (next));
	/* Mark us as running. 
	   running으로 표시 */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif
	// 지금 스레드와 새 스레드가 다르면 -> 스위칭 필요
	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   전환된 스레드가 DYING상태라면, 그 스레드의 struct thread를 파괴한다
		   이 작업은 thread_exit()이 자기 발 밑의 깔개를 갑자기 빼지 않도록 늦게 일어나야 한다.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   현재는 스택에서 사용중이기 때문에 페이지 해제 요청을 큐에 넣기만 한다
		   The real destruction logic will be called at the beginning of the
		   schedule(). 
		   실제 파괴 로직은 schedule()의 시작 부분에서 호출된다
		*/
		// 실행 중인 스택을 당장 free하면 커널이 죽음
		// 따라서 현재는 스택에서 사용중이기 때문에 페이지 해제 요청을 큐에 넣기만 함
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. 
		 스레드를 전환하기 전에, 현재 실행 중인 스레드의 정보를 먼저 저장한다.
		 */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
// 동시에 두 스레드가 접근하는걸 막기위해 락을 걸어둔 용도
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

/* 리스트 정렬 용도. 두 노드 -> 쓰레드의 priority를 비교. */
// 오름차순 정렬시
bool asc_ticks (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct thread *tx = list_entry(x, struct thread, elem);
	struct thread *ty = list_entry(y, struct thread, elem);
	return (tx -> wakeup_tick) < (ty -> wakeup_tick);
}

// 스레드를 sleep큐에 삽입
void thread_sleep(int64_t ticks){
	// 인터럽트를 비활성화해서 동기적으로 처리
	enum intr_level old_level = intr_disable();

	// 현재 스레드 구하기
	struct thread *curr = thread_current();

	// 만약 현재 스레드가 idle 스레드가 아니라면,
	if (curr != idle_thread){

		// 깨울 시간을 저장
		curr->wakeup_tick = ticks;

		// 필요하다면 글로벌 tick을 최소값으로 업데이트하고,
		if (ticks < get_min_ticks()){
			save_min_ticks(ticks);
		}

		// sleep_list에 오름차순으로 삽입
		list_insert_ordered(&sleep_list, &curr->elem, asc_ticks, NULL);

		// 스레드의 상태를 BLOCKED로 전환 + schedule()을 호출
		////////////////////////////// 여기 다시 체크 필요 - 어떤 걸 해야되는지 정확히 파악 후 작성하기 ////////////////////////////
		// thread_block();
		do_schedule(THREAD_BLOCKED);  // -> thread_block이랑 정확히 뭐가 다른거지?
	}
	// 기존 인터럽트 상태로 복원
	intr_set_level (old_level);
}

// 스레드들이 가진 tick 중 최소값을 전역변수에 저장
void save_min_ticks(int64_t new_value){
	min_wakeup_ticks = new_value;
}

// 최소 tick 값을 반환
int64_t get_min_ticks(void){
	return min_wakeup_ticks;
}

static bool dsc_priority (const struct list_elem *x, const struct list_elem *y, const void *aux){
	struct thread *tx = list_entry(x, struct thread, elem);
	struct thread *ty = list_entry(y, struct thread, elem);
	return (tx -> priority) > (ty -> priority);
}


// sleep_list에 들어 있는 스레드들 중에서, 깨울 시간이 된 애들을 깨워서 ready_list로 보내는 함수
// 매 틱마다 sleep_list의 맨 앞부터 "wakeup_ticks<=현재tick?"이면 깨우고, 더 큰 wakeup_ticks만나면 멈춤
// 깨울 애들 다 꺼냈으면 남은 sleep_list 중에서 누가 제일 빨리 깰지 저장해둬야 함(min_wakeup_ticks에 저장)
// void thread_wakeup(){
// 	// 인터럽트 비활성화
// 	enum intr_level old_level = intr_disable();

// 	// sleep_list가 비어있는지 확인해서 비어 있지 않다면
// 	while(!list_empty(&sleep_list)){

// 		// sleep_list의 맨 앞 스레드 꺼내기
// 		struct thread *curr_thread = list_entry(list_front(&sleep_list), struct thread, elem);

// 		// 그 스레드의 wakeup_ticks 확인
// 		int64_t now_ticks = timer_ticks();

// 		// 현재 ticks >= wakeup_ticks이면 - 스레드가 깨어날 시간(wakeup_tick)이 현재 시간이 되었는가?
// 		if (curr_thread->wakeup_tick <= now_ticks){

// 			// sleep_list에서 제거
// 			list_pop_front(&sleep_list);

// 			// thread_unblock으로 깨우기
// 			// thread_unblock : 인터럽트 비활성화+스레드를ready_list에추가+상태를ready로변경+인터럽트 상태를 원래대로 복원
// 			thread_unblock(curr_thread);
// 		}
// 		// else 루프 종료 (아직 깨울 시간이 안 된 스레드가 나오면 그 뒤는 다 깨울 필요 없음)
// 		else{
// 			break;
// 		}
// 	}
		
// 	// sleep_list가 비어있거나 / 다 확인한 경우
// 	if(list_empty(&sleep_list)){
// 	// sleep_list가 비어있으면 가장 큰 값을 넣어두기
// 		min_wakeup_ticks = INT64_MAX;
// 	}
// 	else{
// 		// 아니면 sleep_list의 맨 앞 스레드의 wakeup_ticks로 업데이트 
// 		min_wakeup_ticks = list_entry(list_front(&sleep_list), struct thread, elem)->wakeup_tick;
// 	}
		
// 	// 인터럽트 상태 복원
// 	intr_set_level(old_level);
// }

// sleep_list를 순회하면서 깨울 쓰레드를 찾음
void thread_wakeup(int64_t ticks){
	// interrupt 차단하기
	enum intr_level old_level;
	old_level = intr_disable();

	// 깨울 쓰레드가 있는지 확인하기
	if (ticks >= min_wakeup_ticks){

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
		}

		// global tick을 update
		if (list_empty(&sleep_list)){
			save_min_ticks(INT64_MAX);
		} else {
			save_min_ticks(curr_thread -> wakeup_tick);
		}
	}
	intr_set_level(old_level);
}