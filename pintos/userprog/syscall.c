#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/mmu.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool valid_pointer(void *p);
void exit(int status);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

// [구현 2-2] 포인터가 valid한지 확인하는 함수를 만든다.
bool valid_pointer (void *p){
	// printf("주소: %p\n", p);
	// printf("커널영역여부: %d\n", is_kernel_vaddr(p));
	// printf("매핑되지 않은페이지 여부: %p\n". pml4_get_page(thread_current() -> pml4, p));

	// 널 주소 -> false;
	if (p == NULL) exit(-1);

	// 커널 영역의 주소 -> false
	if (is_kernel_vaddr(p)) exit(-1);

	// 매핑되지 않은 페이지 -> false
	if (pml4_get_page(thread_current() -> pml4, p) == NULL) exit(-1);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// [구현 2-1] 시스템 콜 번호에 따라, 올바른 함수가 실행되도록 구현한다.
	// 인가는 rdi rsi rdx 
	int number = f -> R.rax;

	switch(number){
		case SYS_HALT:					
			/* Halt the operating system. */
			// [구현 3] 핀토스를 종료. 매우 쉽죠?
		  	power_off();
			break;
		case SYS_EXIT:					
			/* Terminate this process. */
			// [구현 4] 쓰레드/프로세스 종료
			exit((int)(f -> R.rdi));
			break;
		case SYS_WRITE:
			/* Write to a file. */
			// [구현 5] 콘솔 창에다 출력하는 부분만 일단 우선 구현 가능.
			valid_pointer(f -> R.rsi);

			if ((int)(f -> R.rdi) == 1){
				putbuf((char *)(f -> R.rsi), (size_t)(f -> R.rdx));
			}
			break;

		
		// case SYS_FORK:					
		//     /* Clone current process. */
		// case SYS_EXEC:                   /* Switch current process. */
		// case SYS_WAIT:                   /* Wait for a child process to die. */
		// case SYS_CREATE:              /* Create a file. */
		// case SYS_REMOVE:                 /* Delete a file. */
		// case SYS_OPEN:                   /* Open a file. */
		// case SYS_FILESIZE:               /* Obtain a file's size. */
		// case SYS_READ:                   /* Read from a file. */
		// case SYS_SEEK:                   /* Change position in a file. */
		// case SYS_TELL:                   /* Report current position in a file. */
		// case SYS_CLOSE:  				/* Close a file. */
	}

	//printf ("system call!\n");
	// thread_exit ();
}

void exit(int status){
	// [구현 2-3] 사용자프로세스 종료
	struct thread *cur = thread_current();
	printf("%s: exit(%d)\n", cur->name, status);
	thread_exit();
}