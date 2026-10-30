#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include <string.h>
#include "threads/malloc.h"
#include "threads/init.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

	filesys_lock_init ();

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}
static bool
is_valid_stack_addr (void *rsp, void *va) {
    uint64_t stack_ptr = (uint64_t) rsp;
    uint64_t addr = (uint64_t) va;

    if (addr < (USER_STACK - (1 << 20)) || addr >= USER_STACK)
        return false;

    if (stack_ptr < 8)
        return false;

    return addr >= stack_ptr - 8;
}
bool
is_valid (struct intr_frame *f UNUSED) {
	struct thread *curr = thread_current ();
	switch (f->R.rax) {
		case SYS_EXEC :
		case SYS_FORK :
		case SYS_CREATE :
		case SYS_REMOVE :
		case SYS_OPEN : {
			const char *fn_copy = (const char*)f->R.rdi;
			while (true) {
				if (is_user_vaddr (fn_copy) && pml4_get_page (curr->pml4, fn_copy)) {
					if (*fn_copy == '\0')
						break;
					fn_copy++;
				}
				else
					return false;
			}
			break;
		}

		case SYS_READ:
		case SYS_WRITE: {
			uint64_t start = f->R.rsi;
			uint64_t len = f->R.rdx;

			if (len == 0)
				break;

			/* NULL, 커널 주소 검사 */
			if (start == 0 || start >= (uint64_t) KERN_BASE)
				return false;

			/* 버퍼 범위 초과 검사 */
			if (len > (uint64_t) KERN_BASE - start)
				return false;

			uint64_t end = start + len - 1;
			uint64_t page_addr =
				(uint64_t) pg_round_down ((void *) start);
			uint64_t last_page_addr =
				(uint64_t) pg_round_down ((void *) end);

			while (page_addr <= last_page_addr) {
				struct page *page =
					spt_find_page (&curr->spt, (void *) page_addr);

				if (page == NULL) {
					/* SPT에 없는 페이지는 스택 증가 가능성 검사 */
					uint64_t check_addr =
						page_addr < start ? start : page_addr;

					if (!is_valid_stack_addr (
							curr->user_rsp, (void *) check_addr))
						return false;
				} else if (f->R.rax == SYS_READ && !page->writable) {
					return false;
				}

				page_addr += PGSIZE;
			}
			break;
		}

		default:
			break;
	}
	return true;
}


void
valid_handler (struct intr_frame *f) {
	if (!is_valid (f)) {
		thread_current ()->ss->exit_status = -1;
		thread_exit ();
		NOT_REACHED ();
	}
}

struct fd_entry *
find_file_discriptor (int fd) {
	struct list_elem *e;
	struct list *fd_table = &thread_current ()->fd_table;
	
	for (e = list_begin (fd_table); e != list_end (fd_table); e = list_next (e)) {
		struct fd_entry *fd_e = list_entry (e, struct fd_entry, elem);

		if (fd_e->fd == fd) 
			return fd_e;
	}
	return NULL;
}

int
count_open_file (struct fd_entry *target_fde) {
	struct list_elem *e;
	struct list *fd_table = &thread_current ()->fd_table;
	int open_cnt = 0;

	for (e = list_begin (fd_table); e != list_end (fd_table); e = list_next (e)) {
		struct fd_entry *fd_e = list_entry (e, struct fd_entry, elem);

		if (fd_e->file == target_fde->file) 
			open_cnt++;
	}

	return open_cnt;
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	struct thread *curr = thread_current ();
	curr->user_rsp = (void *) f->rsp;

	struct fd_entry *fde;
	switch (f->R.rax) {
		case SYS_HALT :
			power_off();
			break;

		case SYS_EXIT :
			curr->ss->exit_status = f->R.rdi;		
			thread_exit ();
			NOT_REACHED ();
			break;

		case SYS_FORK :
			valid_handler (f);
			f->R.rax = process_fork (f->R.rdi, f);
			break;

		case SYS_EXEC : {
			valid_handler (f);
			char *file_name = palloc_get_page (PAL_ZERO);
			if (file_name == NULL) {
				curr->ss->exit_status = -1;
				thread_exit ();
			}
			strlcpy (file_name, f->R.rdi, PGSIZE);
			process_exec (file_name);
			curr->ss->exit_status = -1;
			thread_exit ();
			break;
		}

		case SYS_WAIT :
			f->R.rax = process_wait ((tid_t)f->R.rdi);
			break;

		case SYS_CREATE :
			valid_handler (f);
			f->R.rax = filesys_create (f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE :
			valid_handler (f);
			f->R.rax = filesys_remove (f->R.rdi);
			break;

		case SYS_OPEN : {
			valid_handler (f);
			f->R.rax = -1;
			struct file *file = filesys_open (f->R.rdi);
			if (file == NULL) {
				break;
			}

			fde = malloc (sizeof (struct fd_entry));
			if (fde == NULL) {
				file_close (file);
				break;
			}
			fde->file = file;
			fde->is_stdin = false;

			if (list_empty (&curr->fd_table))
				fde->fd = 2;
			else
				fde->fd = list_entry (list_back (&curr->fd_table), struct fd_entry, elem)->fd + 1;
			list_insert_ordered (&curr->fd_table, &fde->elem, compare_fd, NULL);
			f->R.rax = fde->fd;
			break;
		}

		case SYS_FILESIZE :
			f->R.rax = -1;
			if (f->R.rdi == 0 || f->R.rdi == 1)
				break;
			
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
			
			if (f->R.rdi == fde->fd) {
				f->R.rax = file_length (fde->file);
			}
			break;

		case SYS_READ :	{
			valid_handler (f);
			f->R.rax = -1;
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
			
			if (fde->file == NULL && fde->is_stdin) {
				char *buffer = (char *)f->R.rsi;
				for (size_t i = 0; i < f->R.rdx; i++) 
					buffer[i] = input_getc ();
				f->R.rax = f->R.rdx;
				break;
			}
			else if (fde->file == NULL && !fde->is_stdin) {
				break;
			}

			if (f->R.rdi == fde->fd) {
				f->R.rax = file_read (fde->file, (void *)f->R.rsi, f->R.rdx);
			}
			break;
		}

		case SYS_WRITE :
			valid_handler (f);
			f->R.rax = -1;
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
			
			if (fde->file == NULL && fde->is_stdin)
				break;
			else if (fde->file == NULL && !fde->is_stdin) {
				putbuf ((char *)f->R.rsi, f->R.rdx);
				f->R.rax = f->R.rdx;
				break;
			}
				
			if (f->R.rdi == fde->fd) {
				f->R.rax = file_write (fde->file, f->R.rsi, f->R.rdx);
			}
			break;
		
		case SYS_SEEK :
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
				
			if (fde->file == NULL) 
				break;

			if (f->R.rdi == fde->fd) {
				file_seek (fde->file, f->R.rsi);
			}
			break;

		case SYS_TELL :
			f->R.rax = -1;
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
				
			if (fde->file == NULL) 
				break;

			if (f->R.rdi == fde->fd) {
				f->R.rax = file_tell(fde->file);
			}
			break;

		case SYS_CLOSE :
			fde = find_file_discriptor (f->R.rdi);
			if (fde == NULL)
				break;
			
			int open_cnt = count_open_file (fde);
			if (open_cnt > 1) {
				list_remove (&fde->elem);
				free (fde);
				return;
			}
			
			if (f->R.rdi == fde->fd) {
				file_close (fde->file);
				list_remove (&fde->elem);
				free (fde);
			}
			break;

		case SYS_DUP2 :
			f->R.rax = -1;
			int oldfd = f->R.rdi;
			int newfd = f->R.rsi;
			fde = find_file_discriptor (oldfd);
			if (fde == NULL)
				break;

			if (oldfd == newfd) {
				f->R.rax = newfd;
				break;
			}

			if (newfd < 0)
				break;

			/* 복사할 곳에 fde가 이미 있으면 close하고 복사*/
			struct fd_entry *copy_fde = find_file_discriptor (newfd);
			if (copy_fde != NULL) {
				int open_cnt = count_open_file (copy_fde);
				if (open_cnt < 2) {
					file_close (copy_fde->file);
				}
			}
			/* fde가 없으면 새로 할당 */
			else {
				copy_fde = malloc (sizeof (struct fd_entry));
				if (copy_fde == NULL)
					break;
				copy_fde->fd = newfd;
				list_insert_ordered (&curr->fd_table, &copy_fde->elem, compare_fd, NULL);
			}
			copy_fde->file = fde->file;
			copy_fde->is_stdin = fde->is_stdin;
			f->R.rax = newfd;
			break;

		case SYS_MMAP :
		case SYS_MUNMAP :
		case SYS_CHDIR :
		case SYS_MKDIR :
		case SYS_READDIR :
		case SYS_ISDIR :
		case SYS_INUMBER :
		case SYS_SYMLINK :
		case SYS_MOUNT :
		case SYS_UMOUNT :
			break;

		default :
			break;
	}
	return;
}