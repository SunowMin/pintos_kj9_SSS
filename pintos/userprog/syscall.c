#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
// 헤더 파일 추가 header
#include "threads/init.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081					/* Segment selector msr */
#define MSR_LSTAR 0xc0000082				/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
													((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
						FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{ // 레지스터에 인자들을 저장해서 f의 포인터를 전달 받는다.
	// TODO: Your implementation goes here.

	// int syscall_no = (const int *)f->rsp;
	int syscall_no = (int)f->R.rax;

	switch (syscall_no)
	{
	case SYS_HALT:
		power_off();
		break;

	case SYS_WRITE:
		// f->R.rdi = fd
		// f->R.rsi = buffer
		// f->R.rdx = size
		putbuf(f->R.rsi, f->R.rdx);
		break;

	case SYS_EXIT:
		struct thread *t = thread_current();
		t->exit_arg = f->R.rdi;
		thread_exit();
		break;

	case SYS_EXEC:
		// char *stack = (uint64_t *)f->rsp;
		// char *cmd_line = (char *)stack[1];

		char *fn_cp;
		// char *cmd_line = (char *)f->R.rdi;

		if (!is_user_vaddr(f->R.rdi) || f->R.rdi == NULL)
		{
			f->R.rax = -1;
			break;
		}

		fn_cp = palloc_get_page(0); // process_exec에서 palloc으로 해제하기 떄문에
		strlcpy(fn_cp, f->R.rdi, PGSIZE);
		f->R.rax = process_exec(fn_cp); // 반환값은 rax 레지스터에 저장
		palloc_free_page(fn_cp);
		break;

	default:
		break;
	}
	// printf("system call!\n");
	// thread_exit();
}
