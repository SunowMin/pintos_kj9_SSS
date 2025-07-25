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
#include <string.h>
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "filesys/filesys.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void check_address(void *addr);

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

	// f->R.rdi = fd
	// f->R.rsi = buffer
	// f->R.rdx = size

	switch (syscall_no)
	{
	case SYS_HALT:
		power_off();
		break;

	case SYS_WRITE:
		check_address(f->R.rsi); // 버퍼를 검사?

		if (f->R.rdi == 0 || f->R.rdi >= 64)
		{
			f->R.rax = -1;
		}
		else if (f->R.rdi == 1)
		{
			putbuf(f->R.rsi, f->R.rdx);
			f->R.rax = f->R.rdx;
		}
		else
		{
			struct file *fobj = thread_current()->fdt[f->R.rdi];
			if (fobj == NULL)
			{
				f->R.rax = -1;
			}
			else
			{
				file_write(fobj, f->R.rsi, f->R.rdx);
				f->R.rax = f->R.rdx;
			}
		}
		break;

	case SYS_EXIT:
		struct thread *t = thread_current();
		t->exit_arg = f->R.rdi;
		thread_exit();
		break;

	case SYS_EXEC:

		char *fn_cp;

		check_address(f->R.rdi);

		fn_cp = palloc_get_page(0); // process_exec에서 palloc으로 해제하기 떄문에
		strlcpy(fn_cp, f->R.rdi, PGSIZE);

		f->R.rax = process_exec(fn_cp);
		thread_exit();
		break;

	case SYS_CREATE:
		check_address(f->R.rdi);
		f->R.rax = filesys_create(f->R.rdi, f->R.rdx);
		break;

	case SYS_FORK:
		check_address(f->R.rdi);
		f->R.rax = process_fork(f->R.rdi, f);
		break;

	case SYS_OPEN:
		/* 1. thread 안에 fdt 멤버 추가 */
		/* 2. fdt을 malloc으로 초기화 - process_init */
		/* 3. filesys_open로 열고 반환받은 포인터를 fd에 할당 */
		/* 4. filesys가 null을 반환하면, -1을 반환 */
		check_address(f->R.rdi);
		struct file *fp = filesys_open(f->R.rdi);
		struct thread *cur = thread_current();

		if (fp == NULL)
		{
			f->R.rax = -1; // 파일이 없으면 –1
			break;
		}
		else
		{
			cur->fdt[cur->next_fd] = fp;
			f->R.rax = cur->next_fd;
			cur->next_fd += 1;
		}
		break;

	case SYS_FILESIZE:
		f->R.rax = file_length(thread_current()->fdt[f->R.rdi]);
		break;

	case SYS_CLOSE:
		if (2 <= f->R.rdi && f->R.rdi <= 63)
		{
			file_close(thread_current()->fdt[f->R.rdi]);
			thread_current()->fdt[f->R.rdi] = NULL;
		}
		break;

	default:
		break;
	}
	// printf("system call!\n");
	// thread_exit();
}

void check_address(void *addr)
{
	struct thread *cur = thread_current();
	if (addr == NULL || !(is_user_vaddr(addr)) || pml4_get_page(cur->pml4, addr) == NULL)
	{
		thread_current()->exit_arg = -1;
		thread_exit();
	}
}