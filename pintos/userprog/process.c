#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void __do_fork (void *);
static void initd (void *f_name);

// [구현 7-2] 부모의 인터럽트 프레임 전달 위한 구조체 선언
struct fork_aux {
	struct thread *parent;
	struct intr_frame *parent_if;
	bool success;
};

// [구현 8-10] 부모의 struct thread 전달 위한 구조체 선언
struct init_aux {
	struct thread *parent;
	char *fn_copy;
};



/* General process initializer for initd and other process. */
int
process_init (void) {
	struct thread *current = thread_current ();

	// [구현 7-1] 쓰레드 구조체의 세마포어 초기화
	sema_init(&(current -> f_sema), 0);
	// [구현 8-1] child_info 초기화
	list_init(&(current -> children));
	struct child_info *ci = palloc_get_page(PAL_ZERO);

	if (ci == NULL){
		return -1;
	}

	ci -> parent_alive = true;
	sema_init(&(ci -> w_sema), 0);
	ci -> tid = current -> tid;
	ci -> alive = true;
	ci -> waiting = false;
	current -> ci = ci;

	// [구현 11-1] fdt 초기화
	current -> fdt = calloc(128, sizeof(struct file *));
	if (current -> fdt == NULL){
		return -1;
	}

	current -> next_fd = 2;
	
	// [구현 16-1]
	current -> running = NULL;

	return 0;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	tid_t tid;

	// 버퍼 준비
	char buffer[128];
	char *file_token;
	strlcpy(buffer, file_name, 128);

	// [구현 8-10] 첫 프로세스에 부모의 struct thread 전달하기
	if (process_init() == -1){
		return TID_ERROR;
	};

	struct init_aux *ia = malloc(sizeof(struct init_aux));
	if (ia == NULL){
		return TID_ERROR;
	}

	ia -> fn_copy = palloc_get_page(0);
	if (ia -> fn_copy == NULL){
		return TID_ERROR;
	}
	strlcpy(ia -> fn_copy, file_name, PGSIZE);
	ia -> parent = thread_current();

	char *save_ptr;
	// [구현 1-2] 현재 file_name은 "echo x y z"
    //  file_name은 "echo"만 전달해야 함
	file_token = strtok_r(buffer, " ", &save_ptr);	// 첫 번째 argument만 저장됨 

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_token, PRI_DEFAULT, initd, ia);

	if (tid == TID_ERROR){
		palloc_free_page(ia -> fn_copy);
		free(ia);
		return tid;
	}

	// [구현 8-11] 첫 사용자 프로그램이 자식으로 설정될 때까지 세마포어로 대기.
	sema_down(&(thread_current() -> f_sema));
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *aux_){
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	process_init ();
	struct thread *parent = ((struct fork_aux *)(aux_)) -> parent;
	char *f_name = ((struct init_aux *) aux_) -> fn_copy;
	struct thread *curr = thread_current();
	free(aux_);
	
	// [구현 8-11] 첫 프로세스도 부모자식 관계를 설정해야 함.
	list_push_back(&(parent -> children), &(curr -> ci -> c_elem));
	sema_up(&(parent -> f_sema)); // 설정 완료 후 부모의 대기 해제
	
	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	struct thread *curr = thread_current();
	// [구현 7-2] 자식에게 부모의 인터럽트 프레임 전달하기.
	struct fork_aux *fa = malloc(sizeof(struct fork_aux));
	if (fa == NULL){
		return TID_ERROR;
	}
	fa -> parent = thread_current();
	fa -> parent_if = if_; 
	fa -> success = false;

	/* Clone current thread to new thread.*/
	tid_t result = thread_create(name, PRI_DEFAULT, __do_fork, fa);

	if (result == TID_ERROR){	
		free(fa);
		return TID_ERROR;
	}

	// [구현 7-3] 부모 세마포어 내리기
	sema_down(&(thread_current()->f_sema));


	if (!(fa -> success)){
		result = TID_ERROR;
	}
	free(fa);
	return result;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
// pte는 부모 페이지테이블의 각 엔트리
// va는 해당 엔트리의 가상 주소
// aux는 부모 프로세스 (struct thread) 주소
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *new_page;
	bool writable;

	// [구현 7-4] 부모의 페이지 테이블 복사를 위한, duplicate_pte 함수 완성하기

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 1. 부모 페이지가 커널에 위치한 경우 즉시 반환한다. */
	
	if (is_kern_pte(pte)){
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	/* 2. 부모의 pml4 테이블에 부모 페이지의 주소를 parent_page에 저장한다. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL){
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	new_page = palloc_get_page(PAL_USER);
	if (new_page == NULL){
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(new_page, parent_page, PGSIZE);
	if (is_writable(pte)){
		writable = 1;
	} else {
		writable = 0;
	}

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, new_page, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(new_page);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
// 뭔 말이냐면, 쓰레드의 tf 멤버는 context switching할 때만 갱신됨. 그래서 해당 값은 현재 context를 담고 있지 않음.
// 커널 스택을 직접 꺼내와야 하나?

static void
__do_fork (void *aux) {
	// [구현 7-2] 부모로부터 인터럽트 프레임 전달받기
	struct intr_frame if_;
	struct thread *parent = ((struct fork_aux *)(aux)) -> parent;
	struct thread *current = thread_current ();
	struct intr_frame *parent_if = ((struct fork_aux *)(aux)) -> parent_if;
	
	bool succ = true;

	process_init ();

	/* 1. Read the cpu context to local stack. */
	// 위에서 변수로 선언한 struct intr_frame if_
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt)){
		goto error;
	}
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)){
		goto error;
	}
#endif

	// [구현 7-5] 자식프로세스 반환값 0으로 설정
	if_.R.rax = 0;

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	
	
	thread_current() -> next_fd = parent -> next_fd;

	//  // 부모 프로세스 복사 가능?
	for (int fd = 2; fd <= 127; fd++){
		struct file *parent_file = (parent -> fdt)[fd];
		if (parent_file != NULL){
			struct file *copy = file_duplicate(parent_file);
			if (copy == NULL){
				goto error;
			}
			(thread_current() -> fdt)[fd] = copy;
		}
		
	}
	

	
	/* Finally, switch to the newly created process. */
	if (succ){
		
		((struct fork_aux *)(aux)) -> success = true;
		// [구현 8-2] fork 이후 부모/자식 관계 설정
		list_push_back(&(parent -> children), &(current -> ci -> c_elem));				// 부모의 children 리스트에 자식 삽입

		// [구현 7-6] 자식프로세스 처리 종료 후 세마포어 올리기
		sema_up(&(parent -> f_sema));
		do_iret (&if_);
	}
error:
	((struct fork_aux *)(aux)) -> success = false;
	sema_up(&(parent -> f_sema));
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;
	
	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;


	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	//[구현 8-3] children 리스트에서 child_tid를 가진 자식을 찾기
	struct thread *curr = thread_current();

	struct child_info *child;
	struct list_elem *e;
	int flag = 0;
	int wait_return;

	if (!list_empty(&(curr->children))){
		for (e = list_begin(&(curr -> children)); e != list_end(&(curr -> children)); e = list_next(e)){
			child = list_entry(e, struct child_info, c_elem);
			if (child -> tid == child_tid){
				flag = 1;
				break;
			}
		}
	}

	if (flag == 0 || child -> waiting){
		// 찾지 못한 경우 / 리스트가 빈 경우 -1 반환
		// 동일 child를 대기하는 경우도 -1 반환
		return -1;
	}


	// [구현 8-4] 자식의 종료 전까지 대기 
	// 이미 자식이 죽은 경우, 바로 exit code를 얻기
	if (child -> alive){
		// waiting을 true로 설정하고, 세마포어를 내리기.
		child -> waiting = true;
		sema_down(&(child -> w_sema));
		child -> waiting = false;
	}

	// [구현 8-9] 종료된 자식의 child_info free하기
	// 자식을 리스트에서 없애고, free하고, 자식의 exit 코드 반환.
	wait_return = child -> exit_code;
	list_remove(&(child -> c_elem));
	palloc_free_page(child);

	return wait_return;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	// 사용자 프로세스만 pml4 테이블을 가짐
	bool user_process = curr -> pml4 != NULL;

	process_cleanup ();

	// 사용자 프로세스일 때만 아래 코드를 실행.
	if(user_process){
		// [구현 5-2] exit 및 exit 메시지 띄우기 구현
		printf("%s: exit(%d)\n", curr -> name, curr -> exit_code);

		// [구현 8-5] 자식에게 부모의 죽음 알리기
		struct list_elem *e;
		// 모든 자식의 parent_alive를 false처리
		if (!list_empty(&(curr->children))){
			for (e = list_begin(&(curr -> children)); e != list_end(&(curr -> children)); e = list_next(e)){
				list_entry(e, struct child_info, c_elem) -> parent_alive = false;
			}
		}

		for (int fd = 2; fd < 128; fd++){
			file_close((curr -> fdt)[fd]);
		}

		// [구현 뭐였지] file descriptor table 할당해제
		free(curr -> fdt);
		curr -> fdt = NULL;

		// [구현 16-3] 파일 종료 시 다시 실행 허용
		if (curr -> running != NULL){
			file_close(curr -> running);
			curr -> running = NULL;
		}

		// [구현 8-6] 부모가 죽은 경우 child_info free
		if (!(curr -> ci -> parent_alive)){
			// 부모가 죽은 경우, 바로 여기서 free
			palloc_free_page(curr -> ci);
		} else {
			// [구현 8-7] 자신의 child_info 수정
			curr -> ci -> alive = false;	
			curr -> ci -> exit_code = curr -> exit_code;

			// [구현 8-8] 세마포아를 올려 부모의 대기 해제
			sema_up(&(curr -> ci -> w_sema));
		}

	}
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}	
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());
	/* Open executable file. */

	// [구현 1-4: file_name 맨 앞 argument만 parse해야 함]
	char *file_token, *dummy_ptr;
	char bufferA[128];   // 버퍼
	
	strlcpy(bufferA, file_name, 128);

	file_token = strtok_r(bufferA, " ", &dummy_ptr);	// 첫 번째 argument만 저장됨

	file = filesys_open (file_token);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_token);
		goto done;
	}
	// [구현 16-1]
	file_deny_write(file);
	thread_current() -> running = file;

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	// [구현 1-3] 인자 스택에 넣기 (스택주소는 if_->rsp)
	// 문자열 자체는 정순, 주소는 역순으로 넣는 식으로 구현해보자

	uint64_t argc = 0;		// 인자의 수
	char bufferB[128];   // 버퍼
	strlcpy(bufferB, file_name, 128);
	char* argv[30];	// 스택 내 인자의 주소 저장

	// 1단계. 문자열 정순으로 푸시한다
	char *token, *save_ptr;
	for (token = strtok_r(bufferB, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)){
		if_->rsp -= strlen(token) + 1;
		memcpy((void *)if_->rsp, token, strlen(token) + 1);
		argv[argc] = (char*)if_->rsp;
		argc += 1;
		
	}
	argv[argc] = NULL;	// 마지막 널주소

	// 2단계. 패딩 바이트를 추가한다 (사실 이미 초기화할때 0이라 스택 포인터만 올리면 됨)
	int padding = if_->rsp % 8;
	if_->rsp -= padding;

	// 3단계. 문자열이 저장된 주소를 역순으로 푸시한다
	for (int i = argc; i >= 0; i--){
		if_->rsp -= 8;
		memcpy((void *)if_->rsp, &argv[i], 8);
	}

	// 4단계. 널 주소를 푸시한다 (사실 이미 초기화할때 0이라 스택 포인터만 올리면 됨)
	if_->rsp -= 8;

	// 5단계. 레지스터를 갱신한다
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp + 8;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	//file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;


	// 실제 물리 프레임을 OS 물리 메모리 풀에서 할당
	// 이 물리 프레임에 대응하는 커널 가상 주소를 반환
	// kpage: 커널 공간에서, 할당된 물리 페이지에 접근할 수 있는 가상주소
	kpage = palloc_get_page (PAL_USER | PAL_ZERO);	// 사용자 페이지 할당, 모든 바이트는 0으로 초기화. 이때 한 페이지는 4KB.
	

	if (kpage != NULL) {
		// 사용자의 가상 주소 공간에 페이지 테이블을 설정
		// 사용자 가상 주소 (USER_STACK - PGSIZE(4KB))에, 커널이 가진 물리 페이지 (kpage)를 매핑
		// true: 사용자 process가 페이지에 쓰기 가능
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
