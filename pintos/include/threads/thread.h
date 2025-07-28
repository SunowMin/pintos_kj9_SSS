#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <bitmap.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */

struct child_info {
	tid_t tid;				// 쓰레드 번호
	struct list_elem c_elem;	// 리스트 삽입용
	int exit_code;			// 삭제코드 저장
	struct semaphore w_sema;	
	bool alive;				// 생존 여부
	bool parent_alive;		// 부모의 생존 여부
	bool waiting;			// 부모가 해당 프로세스를 대기 중인지
};

struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	/* Project 2에서 추가한 코드 좀 많음. */
	uint64_t *pml4;                     /* Page map level 4 */

	// exit 시스템 콜에 필요
	int exit_code;						/* exit 시스템 콜 시 반환할 코드. */
	
	// fork 시스템 콜에 필요
	struct semaphore f_sema;			/* fork 시스템 콜 용도 */

	// wait 시스템 콜에 필요
	struct list children;				/* 자식 프로세스의 리스트 */
	struct child_info *ci;

	// // file 관련 시스템 콜에 필요
	struct file **fdt;			/* file descriptor 테이블 */
	int next_fd;						/* file descriptor - 다음은 몇번째? */
	struct file *running;				/* 현재 실행 중인 파일 */

	#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	int64_t wakeup_tick;                    /* sleep 중일 때, 깨어날 시간. */

	// [구현 3-1] struct thread 구조체에 priority donation 멤버 추가.
	int saved_priority;					// priority donation이 이루어진 경우, donation 이전 priority.
	struct lock *wait_on_lock;			// 현재 쓰레드가 대기 중인 lock
	struct list donations;				// 현재 쓰레드에 priority를 기부할 수 있는 쓰레드의 리스트.
	struct list_elem d_elem;			// donations 리스트에 집어넣기 위한 용도

	// [구현 4-1] struct thread 구조체에 mlfqs 멤버 추가
	int nice;
	int recent_cpu; 

	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;


extern struct thread *idle_thread;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_schedule(int status);

void thread_sleep(int64_t);
void wake_up(int64_t);
void save_min_ticks(int64_t);
int64_t get_min_ticks(void);

void do_iret (struct intr_frame *tf);

bool asc_ticks (const struct list_elem *x, const struct list_elem *y, const void *aux);
bool dsc_priority (const struct list_elem *x, const struct list_elem *y, const void *aux);

void check_front_yield(void);
void recent_cpu_up_one(void);
void calc_load_avg(void);
void calc_recent_cpu(struct thread *t);
void calc_priority(struct thread *t);
void calc_all_recent_cpu(void);
void calc_all_priority(void);

#endif /* threads/thread.h */
