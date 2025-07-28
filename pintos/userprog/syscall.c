#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "userprog/process.h"
#include <string.h>
#include "threads/palloc.h"
#include "threads/mmu.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "kernel/console.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool valid_pointer (void *p);

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

// 사용자가 넘긴 포인터가 유효한 포인터인지 검사
bool valid_pointer (void *p){
	// 널 주소 / 커널 영역 주소 / 매핑되지 않은 페이지
	if (p == NULL || is_kernel_vaddr(p) || pml4_get_page(thread_current()->pml4, p) == NULL){
		thread_current() -> exit_arg = -1;
		thread_exit();
	}
}

// 사이즈만큼 들어온 버퍼 전체가 유효한 유저 주소인지 확인
void check_valid_buffer(void *buffer, unsigned size) {
    uint8_t *ptr = (uint8_t *)buffer;
    for (unsigned i = 0; i < size; i++) {
        valid_pointer(ptr + i);  // 각 바이트 주소가 유효한 유저 주소인지 확인
    }
}

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

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	// printf ("system call!\n");
	/* 기존 코드 : 시스템 콜이 불리면 바로 종료
	thread_exit ();
	*/

	// 시스템 콜 번호 가져오기
	int sysNum = f->R.rax;

	switch(sysNum){
		// halt 구현
		case SYS_HALT:
			power_off();
			break;

		case SYS_EXIT:
			// exit : 사용자 프로그램을 종료하고 status 커널로 돌아감
			struct thread *curr = thread_current();
			// 부모 프로세스에게 자식의 종료 상태를 전달하기 위해 값을 저장
			curr->exit_arg = f->R.rdi;
			thread_exit();
			break;

		case SYS_EXEC:
			// 현재 프로세스를 cmd_line에 지정된 실행파일로 바꿈
			valid_pointer(f -> R.rdi);
			// char *file_name = (char*) f->R.rdi;
			// // 프로그램 실행할 수 없는 경우에만 -1 반환, 성공하면 반환X
			// if((process_exec(file_name)) == -1){
			// 	f->R.rax = -1;
			// }
			// break;

			// process_exec에서 f_name이 palloc_free_page로 메모리 할당 해제가 이루어지기 때문에
			char *f_name = palloc_get_page(0);
			strlcpy(f_name, f->R.rdi, PGSIZE);
			// 프로그램 실행할 수 없는 경우에만 -1 반환, 성공하면 반환X
			// if((process_exec(file_name)) == -1){
			// 	f->R.rax = -1;
			f -> R.rax = process_exec(f_name);
			thread_exit();
			break;

		case SYS_FORK:
			// [구현 7-7] 자식 프로세스 생성 및 복제
			valid_pointer(f->R.rdi);
			f -> R.rax = process_fork((char *)(f->R.rdi), f);
			break;

		case SYS_CREATE:
			// create (const char *file, unsigned initial_size)
			// 이름이 file인 새 파일을 initial_size 바이트 크기로 생성
			// 반환값 : 성공하면 true, 실패하면 false 
			valid_pointer(f -> R.rdi);
			f->R.rax = filesys_create((const char *)f->R.rdi, (off_t)f->R.rsi);
			break;

		case SYS_REMOVE:
			// bool remove (const char *file)
			valid_pointer(f -> R.rdi);
			f->R.rax = filesys_remove((const char *)f->R.rdi);
			break;

		case SYS_OPEN:
			// struct file *filesys_open (const char *name)
			valid_pointer(f -> R.rdi);

			// const char *f_name = (const char*) f->R.rdi;
			// [open 구현2] 파일 열기 
			// filesys_open하고 반환된 파일 포인터를 변수화
			struct file *fp_open = filesys_open((const char*) f->R.rdi);
			// 파일 열기 실패 시(fp가 null이면) -1 반환 
			if(!fp_open){
				f->R.rax = -1;
				break;
			}

			// [open 구현3] 현재 스레드의 파일 테이블에 삽입
			struct thread *cur = thread_current();
			int fd = cur -> next_fd;

			(cur -> fd_table)[fd] = fp_open;

			// fd 리턴
			f->R.rax = fd;

			// [open 구현4] 다음 fd 번호 증가
			cur->next_fd++;

			break;

		case SYS_FILESIZE:
		// int filesize (int fd) {return syscall1 (SYS_FILESIZE, fd);
			int len = file_length(thread_current ()->fd_table[f -> R.rdi]);
			f->R.rax = len;
			break;

		case SYS_CLOSE:
			int fd_close = f->R.rdi;

			if (1 <=  fd_close || fd_close >= 63){
				break;
			}

			file_close(thread_current() -> fd_table[fd_close]);

			thread_current() -> fd_table[fd_close] = NULL;

			break;

		case SYS_READ:
			// read (int fd, void *buffer, unsigned size) {return syscall3 (SYS_READ, fd, buffer, size);
			// 버퍼(f->R.rsi)의 모든 바이트가 유저 영역인지 확인해야 함
			check_valid_buffer(f->R.rsi, f->R.rdx);

			int fd_read = f -> R.rdi;

			// 현재 스레드 가져옴
			struct thread *cur_read = thread_current();
			
			// fd가 0이면(stdin) input_getc() 사용해서 키보드에서 읽기
			if (fd_read == 0){
				// uint8_t input_key = input_getc();
				// 입력된 값을 사용자 버퍼에 저장
				// f->R.rsi = input_key();
				int i = 0;
				// 반복문으로 진행할거면 문자열 끝(EOF)인지/파일사이즈크기만큼 확인해줘야 함
				// for (i=0; i<f->R.rdx; i++){
				// 	((uint8_t *)f->R.rsi)[i] = input_getc();
				// }
				f->R.rax = input_getc();
				// 반복문 진행 시 적용 - 읽은 바이트 수 반환
				// f->R.rax = i;
			}
			else if(2>fd_read || fd_read>127){
				break;
			}
			// fd가 0이 아닌 실제 파일이면 file_read() 사용
			else{
				// 현재 스레드의 fdt에 접근해서 fp 알아내기
				struct file *fp_read = cur_read -> fd_table[fd_read];
				// 0이면 파일 끝 도달 (EOF), -1이면 읽기 실패, >0이면 그만큼 읽음
				if (fp_read != NULL){
					f->R.rax = file_read(fp_read, f->R.rsi, f->R.rdx);
				}
			}
			break;

		case SYS_WRITE:
		// 실제로 쓰여진 바이트 수를 반환하는 역할
		// int write (int fd, const void *buffer, unsigned size) {return syscall3 (SYS_WRITE, fd, buffer, size);}
			valid_pointer(f->R.rsi);

			// 1. fd 범위 검사
			if (f->R.rdi ==0 || f->R.rdi > 128){
				break;
			}
			// 2. stdout을 호출하는 경우
			else if(f->R.rdi == 1){
				// BUFFER로부터 N바이트를 콘솔에 출력합니다
				putbuf(f->R.rsi, f->R.rdx);
				f->R.rax = f->R.rdx;
			}
			// 3. 정상적인 fd가 입력된 경우
			else{
				struct file *fobj = thread_current()->fd_table[f->R.rdi];
				file_write(fobj, f->R.rsi, f->R.rdx);
				f->R.rax = f->R.rdx;
			}


			// // 표준출력(stdout, fd==1)이 입력된 경우
			// if (f->R.rdi == 1){
			// 	// BUFFER로부터 N바이트를 콘솔에 출력합니다
			// 	putbuf(f->R.rsi, f->R.rdx);
			// 	// 리턴 값 설정
			// 	f->R.rax = f->R.rdx;
			// }
			// else if(2 <= f->R.rdi && f->R.rdi <= 127){
			// 	struct file *fobj = thread_current()->fd_table[f->R.rdi];
			// 		f->R.rax =file_write(fobj, f->R.rsi, f->R.rdx);
			// }
			break;

		case SYS_SEEK:
			int fd_seek = (int)(f -> R.rdi);
			unsigned seek_position = (unsigned)(f->R.rsi);
			if (2 <= fd_seek && fd_seek <=127){
				file_seek((thread_current()->fd_table)[fd_seek], (off_t)(seek_position));
			}
			break;
		
		case SYS_TELL:
			int fd_tell = (int)(f->R.rdi);
			if (2 <= fd_seek && fd_seek <=127){
				f -> R.rax = (off_t)file_tell((thread_current()->fd_table)[fd_tell]);
			}
			break;


			
	}
}