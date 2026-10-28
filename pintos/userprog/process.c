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
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/malloc.h"
#include "vm/vm.h"
#ifdef VM
#include "vm/vm.h"
#include "hash.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	int len = strcspn(file_name, " ");
	char *name = malloc (len + 1);
	if (name == NULL) {
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
	strlcpy (name, file_name, len + 1);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	free (name);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* Clone current thread to new thread.*/
	struct fork_struct fork_struct;
	fork_struct.parent = thread_current ();
	fork_struct.if_ = if_;
	sema_init (&fork_struct.sema, 0);
	
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, &fork_struct);
	if (tid == TID_ERROR)
		return TID_ERROR;

	sema_down (&fork_struct.sema);

	/* fork실패시 자식의 종료 정보 제거 */
	if (!fork_struct.success) {
		process_wait (tid);
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (!parent->pml4)
		return false;
	
	/* 커널 페이지인경우 건너뛰고 다음 페이지 복사 */
	if (is_kernel_vaddr(va))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page (PAL_USER);

	/* 페이지 할당에 실패하거나 부모 페이지가 없는 경우 */
	if (!newpage || !parent_page)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */

	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page (newpage);
		return false;
	}

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	enum intr_level old_level = intr_disable ();
	struct intr_frame if_;
	struct fork_struct *fs = (struct fork_struct *) aux;
	struct thread *parent = fs->parent;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = fs->if_;
	bool succ = true;
	
	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	/* 2. Duplicate PT */
	current->pml4 = pml4_create ();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
	if (parent->exec_file != NULL) {
		filesys_lock_acquire ();
		current->exec_file = file_duplicate (parent->exec_file);
		filesys_lock_release ();
		if (current->exec_file == NULL)
			goto error;
	}
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	intr_set_level (old_level);
	while (!list_empty (&current->fd_table)) {
		struct fd_entry *fde_copy = list_entry(list_pop_front (&current->fd_table), struct fd_entry, elem);
		free (fde_copy);
	}
	for (struct list_elem *e = list_begin (&parent->fd_table); e != list_end (&parent->fd_table); e = list_next (e)) {
		struct fd_entry *fde_copy = malloc (sizeof (struct fd_entry));
		if (fde_copy == NULL)
			goto error;
		struct fd_entry *fde = list_entry (e, struct fd_entry, elem);
		fde_copy->fd = fde->fd;
		fde_copy->is_stdin = fde->is_stdin;
		filesys_lock_acquire ();
		if (fde->file != NULL) {
			fde_copy->file = file_duplicate (fde->file);
			filesys_lock_release ();
			if (fde_copy->file == NULL) {
				free (fde_copy);
				goto error;
			}
		}
		else {
			fde_copy->file = NULL;
			filesys_lock_release ();
		}

		list_insert_ordered (&current->fd_table, &fde_copy->elem, compare_fd, NULL);
	}
	process_init ();
	
	/* Finally, switch to the newly created process. */
	if (succ) {
		if_.R.rax = 0;
		fs->success = true;
		sema_up (&fs->sema);
		do_iret (&if_);
	}
error:
	fs->success = false;
	sema_up (&fs->sema);
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
	memset(&_if, 0, sizeof _if);
	
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
	struct thread *curr = thread_current ();
	struct list_elem *e;	
	struct list *c_list = &curr->children;	

	for (e = list_begin (c_list); e != list_end (c_list); e = list_next (e)) {
		struct shared_status *ss = list_entry(e, struct shared_status, elem);

		if (ss->tid == child_tid) {
			sema_down(&ss->sema);
			list_remove(e);

			int status = ss->exit_status;

			enum intr_level old_level = intr_disable();

			ss->ref_cnt--;								
			if (ss->ref_cnt == 0)
				free(ss);

			intr_set_level(old_level);
			return status;
		}
	}
	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();				
	struct shared_status *self_ss;			

	if (curr == NULL) 
		return;	

	uint64_t *pml4 = curr->pml4;
	struct list *c_list = &curr->children;
	struct list *fd_table = &curr->fd_table;				

	while (!list_empty(c_list)) {
		enum intr_level old_level = intr_disable ();

		struct list_elem *e = list_pop_front (c_list);
		struct shared_status *child_ss = list_entry (e, struct shared_status, elem);
				
		child_ss->ref_cnt--;							
		if (child_ss->ref_cnt == 0) 
			free (child_ss);		

		intr_set_level(old_level);
	}

	filesys_lock_acquire ();
	while (!list_empty(fd_table)) {
		int dup_cnt = 0;
		struct list_elem *e = list_front (fd_table);
		struct fd_entry *fde = list_entry (e, struct fd_entry, elem);

		for (e = list_next (e); e != list_end (fd_table); e = list_next (e)) {
			struct fd_entry *fde_next = list_entry (e, struct fd_entry, elem);
			if (fde->file == fde_next->file && fde->is_stdin == fde_next->is_stdin)
				dup_cnt++;
		}

		if (dup_cnt > 0) {
			list_remove (&fde->elem);
			free (fde);
			continue;
		}
			
		list_remove (&fde->elem);
		file_close (fde->file);
		free (fde);
	}
	filesys_lock_release ();
	
	self_ss = curr->ss;
	process_cleanup ();

	if (self_ss == NULL) 	
		return;	
		
	enum intr_level old_level = intr_disable();

	if (pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, self_ss->exit_status);

	sema_up(&self_ss->sema);	 

	self_ss->ref_cnt--;		
	if (self_ss->ref_cnt == 0)
		free(self_ss);			

	intr_set_level(old_level);			
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif
	/* 실행중인 파일은 쓰기금지, 따로 보관하므로 fd_table이 아니라서 cleanup시 정리*/
	if (curr->exec_file != NULL) {
		struct file *file = curr->exec_file;
		curr->exec_file = NULL;
		filesys_lock_acquire ();
		file_close (file);
		filesys_lock_release ();
	}

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
	char *copy = NULL;
	int i;
	/*Argument Passing 변수 (종민)*/
	char *argv[128];
	char *save_ptr;
	int argc = 0;
	char *token;

	bool fs_locked = false;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/*문자열 복제*/
	copy = copy_file_name (file_name);

	if (copy == NULL)
		goto done;

	/*문자열 파싱*/
	for (token = strtok_r (copy, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr))
		argv[argc++] = token;

	/* Open executable file. */
	filesys_lock_acquire ();
	file = filesys_open (argv[0]);
	if (file != NULL)
		file_deny_write (file);
	filesys_lock_release ();
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* Read and verify executable header. */
	filesys_lock_acquire ();
	fs_locked = true;
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}
	filesys_lock_release ();
	fs_locked = false;

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		filesys_lock_acquire ();
		fs_locked = true;
		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		filesys_lock_release ();
		fs_locked = false;
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

	/*인자를 사용자 스택에 복사*/
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;

		if_->rsp -= len;
		memcpy ((void *) if_->rsp, argv[i], len);
		argv[i] = (char *) if_->rsp;
	}

	/* padding */
	if_->rsp &= ~((uintptr_t) 0x7);

	/* 문자열 주소 배열 종료 */
	if_->rsp -= sizeof(char *);
	*(char **) if_->rsp = NULL;

	/*main이 사용할 복사한 문자열의 주소를 사용자 스택에 입력*/
	for (int j = argc - 1; j >= 0; j--) {
		if_->rsp -= sizeof(char *);
		*(char **)if_->rsp = argv[j];
	}

	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp;

	/* 가짜 반환 함수 */
	if_->rsp -= sizeof (uintptr_t);
	*(uintptr_t *) if_->rsp = 0;

	t->exec_file = file;
	file = NULL;
	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (fs_locked)
		filesys_lock_release ();
	
	free (copy);
	filesys_lock_acquire ();
	file_close (file);
	filesys_lock_release ();
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

/* 문자열 복사, 이후 free 필요 */
char *
copy_file_name (const char *file_name) {
	size_t size = strlen (file_name) + 1;
	char *copy = malloc (size);
	if (copy == NULL)
		return NULL;
	strlcpy (copy, file_name, size);
	return copy;
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

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
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

	/* TODO 1: aux에서 파일 읽기에 필요한 정보를 꺼낸다. */
	struct file_info *info = aux;

    /* TODO 2: 내용을 써넣을 프레임의 커널 가상 주소를 구한다. */
	void *kva = page->frame->kva; 

    /* TODO 3: 파일 시스템 락을 잡고,
     * 지정한 파일의 지정한 오프셋에서 지정한 크기만큼 읽는다.
     * 실제로 읽은 바이트 수를 저장하고 락을 해제한다. */
	filesys_lock_acquire();
	off_t bytes = file_read_at (
		info->file,
		kva,
		info->page_read_bytes,
		info->ofs
	);
	filesys_lock_release();

    /* TODO 4: 요청한 크기만큼 읽었는지 확인한다.
     * 실패했다면 이 함수가 소비하는 보조 정보를 해제하고
     * 실패를 반환한다. */
	if (bytes != (off_t) info->page_read_bytes) {
		free(info);
		return false;
	}

    /* TODO 5: 파일 내용을 읽어 넣은 영역 바로 뒤부터,
     * 남은 영역을 0으로 채운다. 다음 값을 노출시키지 않기 위해(?) */
	memset((uint8_t *) kva + info->page_read_bytes, 0, info->page_zero_bytes);

    /* TODO 6: 더 이상 필요 없는 보조 정보를 해제하고
     * 성공을 반환한다. */
	free(info);
	return true;
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
		struct file_info *info = malloc (sizeof *info);
		if (info == NULL)
			return false;

		info->file = file;
		info->ofs = ofs;
		info->page_read_bytes = page_read_bytes;
		info->page_zero_bytes =page_zero_bytes;

		/* SPT에 페이지와 최초 로딩 방법을 등록. RAM 확보와 매핑은 나중에 수행. */
		if (!vm_alloc_page_with_initializer 
			(VM_ANON, upage,writable, lazy_load_segment, info)){
			free(info);
			return false;
		}
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
// 만든 인자를 넣어주는데, 인자를 넣을 스택을 미리 만들어주는 것
// user program은 쓰였는데 vm에 오면서 없어짐.
// alloc_page_init 써서 페이지 하나를 만들고 SPT에 등록을 시켜놓는다.
// frame을 확보 후, 매핑을 시켜준다.
// user program에서 쓰던 setup_stack랑 같은 동작을 하게.
// rsp를 제일 위로 올려준다.
// top-down으로 이거부터 한다.
static bool
setup_stack (struct intr_frame *if_) {
	bool success = true;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE); //stack용 페이지 주소.

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	/*uninit 페이지로 생성. 지금 쓰려고 init NULL로 넣어줌*/
	if (!vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)) {
		return false;
	}
	//만들고 바로 매핑 시켜줌
	spt_lock_acquire();

	if (!vm_claim_page(stack_bottom)){
		struct page *page = spt_find_page(&thread_current()->spt, stack_bottom);
        if (page != NULL) {
            spt_remove_page(&thread_current()->spt, page);
        }
        return false;
	}

	spt_lock_release();
	
	if_->rsp = USER_STACK;
	return true;
}
#endif /* VM */
