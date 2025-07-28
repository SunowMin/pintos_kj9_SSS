#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/mmu.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void valid_pointer(void *p);
void exit(int status);
static int find_empty_fd(int next_fd);
struct lock filesys_lock;


static int find_empty_fd(int next_fd){
	// next_fd부터 시작해서, fd 리스트에서 빈 곳을 찾기
	int curr_fd = next_fd < 2 ? 2 : next_fd;
	for (int i = 0; i < FD_LIMIT; i++){
		// 0, 1은 pass
		if (2 <= curr_fd){
			if ((thread_current() -> fdt)[curr_fd] == NULL){
				return curr_fd;
			}
		}
		curr_fd = (curr_fd + 1) % FD_LIMIT;
	}
	// 꽉 찬 경우
	return -1;
}

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

	lock_init(&filesys_lock);
}

// [구현 2-2] 포인터가 valid한지 확인하는 함수를 만든다.
void valid_pointer (void *p){
	// 널 주소-/ 커널 영역의 주소 / 매핑되지 않은 페이지
	if (p == NULL || is_kernel_vaddr(p) || pml4_get_page(thread_current() -> pml4, p) == NULL){
		thread_current() -> exit_code = -1;
		thread_exit();
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// [구현 2-1] 시스템 콜 번호에 따라, 올바른 함수가 실행되도록 구현한다.
	// 인자는 rdi rsi rdx ....
	int number = f -> R.rax;

	switch(number){
		case SYS_HALT:					
			/* Halt the operating system. */
			// [구현 3] 핀토스를 종료. 매우 쉽죠?
		  	power_off();
			break;
		case SYS_WRITE:
			/* Write to a file. */
			// [구현 4-1] fd가 1일 땐 콘솔창으로 출력한다
			valid_pointer(f -> R.rsi);
			int write_fd = (int)(f -> R.rdi);

			if (write_fd == 1){
				putbuf((char *)(f -> R.rsi), (size_t)(f -> R.rdx));
			} else if (2 <= write_fd && write_fd <= 127) {
				lock_acquire(&filesys_lock);
				struct file *write_file = (thread_current() -> fdt)[write_fd];
				if (write_file != NULL){
					f -> R.rax = file_write(write_file, f -> R.rsi, f -> R.rdx);
				}
				lock_release(&filesys_lock);
			}
			break;
		case SYS_EXIT:					
			/* Terminate this process. */
			// [구현 5-1] thread 내 exit_code 멤버에 status 저장
			thread_current() -> exit_code = (int)(f -> R.rdi);
			thread_exit();
			break;
		case SYS_EXEC:
			/* Switch current process. */
			// [구현 6] 새로운 프로세스 실행
			// 성공할 시 return하지 않음에 유의
			valid_pointer(f -> R.rdi);
			char *f_name = palloc_get_page(0);
			strlcpy(f_name, f->R.rdi, PGSIZE);
			f -> R.rax = process_exec(f_name);
			thread_current() -> exit_code = -1;
			thread_exit();
			break;
		case SYS_FORK:
			/* Clone current process. */
			// [구현 7-7] 자식 프로세스 생성 및 복제
			valid_pointer(f -> R.rdi);
			f -> R.rax = process_fork((char *)(f -> R.rdi), f);
			break;
		
		case SYS_WAIT:
			/* Wait for a child process to die. */
			// [구현 8-7] 자식 프로세스를 기다린다.
			f -> R.rax = process_wait(f -> R.rdi);
			break;
		case SYS_CREATE:
			/* Create a file. */
			// [구현 9]	파일을 만든다.
			valid_pointer(f -> R.rdi);
			lock_acquire(&filesys_lock);
			f -> R.rax = filesys_create((char *)(f -> R.rdi), f -> R.rsi);
			lock_release(&filesys_lock);
			break;
		case SYS_REMOVE:
			/* Delete a file. */
			// [구현 10] 파일을 삭제한다.
			valid_pointer(f -> R.rdi);
			lock_acquire(&filesys_lock);
			f -> R.rax = filesys_remove((char *)(f -> R.rdi));
			lock_release(&filesys_lock);
			break;
		case SYS_OPEN:
			/* Open a file. */
			// [구현 11-3] 파일 오픈 후 반한된 file 포인터를 fdt 테이블에 삽입한 뒤, 식별자 번호를 반환한다.
			struct file *opened_file;
			valid_pointer(f -> R.rdi);
			lock_acquire(&filesys_lock);
			int set_fd = find_empty_fd(thread_current() -> next_fd);
			if (set_fd == -1){
				f -> R.rax = -1;
			} else if (2 <= set_fd && set_fd <= 127) {
				opened_file = filesys_open((char *)(f -> R.rdi));
				if (opened_file == NULL){
					// 파일 열기에 실패한 경우 -1 반환
					f -> R.rax = -1;
				} else {
					// 파일 열기에 성공한 경우 fdt에 포인터 삽입하기
					(thread_current() -> fdt)[set_fd] = opened_file;
					// 파일 식별자 반환
					f -> R.rax = set_fd;
					// 다음 파일 식별자 값 1 증가
					thread_current() -> next_fd = (set_fd + 1) % FD_LIMIT;
				}
			}
			lock_release(&filesys_lock);
			break;

		case SYS_FILESIZE:               
			/* Obtain a file's size. */
			// [구현 12] 파일 식별자에 대응되는 파일의 크기를 반환한다.
			int size_fd = (int)(f -> R.rdi);
			if (2 <= size_fd && size_fd <= 127){
				lock_acquire(&filesys_lock);
				f -> R.rax = file_length(thread_current() -> fdt[(int)(f -> R.rdi)]);
				lock_release(&filesys_lock);
			}
			
			break;
		case SYS_CLOSE:  				
			/* Close a file. */
			// [구현 13] 파일을 닫는다. fdt에서 NULL로 바꿔준다.
			int close_fd = (int)(f -> R.rdi);
			// fdt에 할당된 범위의 fd만 닫을 수 있게 한다.
			if (2 <= close_fd && close_fd <= 127){
				lock_acquire(&filesys_lock);
				struct file *close_file = (thread_current() -> fdt)[close_fd];
				
				file_close(close_file);
				(thread_current() -> fdt)[close_fd] = NULL;
				lock_release(&filesys_lock);
			}
			break;
		case SYS_SEEK:                   
			/* Change position in a file. */
			// [구현 14] fd에서 읽을 다음 위치를 변경한다.
			int seek_fd = (int)(f -> R.rdi);
			unsigned seek_position = (unsigned)(f -> R.rsi);
			if (2 <= seek_fd && seek_fd <= 127){
				file_seek((thread_current() -> fdt)[seek_fd], (off_t)(seek_position));
			}
			break;
		case SYS_TELL:                   
			/* Report current position in a file. */
			// [구현 15] fd에서 읽을 다음 위치를 반환한다.
			int tell_fd = (int)(f -> R.rdi);
			
			if (2 <= tell_fd && tell_fd <= 127){
				f -> R.rax = (off_t)file_tell((thread_current() -> fdt)[tell_fd]);
			}
			break;
		case SYS_READ:
			int read_fd = (int)(f -> R.rdi); 
			// [구현 16-1] fd가 0일 땐 stdin, 키보드에서 읽는다
			if (read_fd == 0){
				// 이게 맞나?? 개선 필요.
				f -> R.rax = (int)input_getc();
			} else if (2 <= read_fd && read_fd <= 127) {
				// [구현 16-2] fd가 0이 아닐 땐 파일에서 읽는다
				lock_acquire(&filesys_lock);
				valid_pointer(f -> R.rsi);
				struct file *read_file = (thread_current() -> fdt)[read_fd];
				if (read_file != NULL){
					f -> R.rax = (int)file_read(read_file, f -> R.rsi, f -> R.rdx);
				}
				lock_release(&filesys_lock);
			}
			break;
	}
}