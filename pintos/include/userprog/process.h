#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

struct shared_status {

	tid_t tid;
	int exit_status;
	int ref_cnt;
	
	struct semaphore sema; 
	struct list_elem elem;
};

struct fork_struct {

	struct thread *parent;
	struct intr_frame *if_;
  struct semaphore sema;
  bool success;
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

char *copy_file_name (const char *file_name);

#endif /* userprog/process.h */
