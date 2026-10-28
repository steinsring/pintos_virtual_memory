/* vm.c: Generic interface for virtual memory objects. */
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
// 아직 최초 초기화를 하지 않은 페이지
vm_init (void) {
	// 이 페이지의 현재 내용을 보존할 원본 파일이 없다. 내보낼 때 swap이 필요하다.
	vm_anon_init ();
	// 이 페이지를 뒷받침하는 파일이 있다. 필요한 변경을 그 파일에 반영하고, 나중에 파일에서 다시 읽는다.
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
// 가상 페이지를 관리하겠다.
// 페이지를 지금 RAM에 올리는 함수 X 나중에 준비할 페이지의 정보를 만들어 SPT에 등록하는 함수
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check whether the upage is already occupied or not. */
	spt_lock_acquire();

	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *page = malloc (sizeof *page);
	
		if (page==NULL)
			goto err;

		bool(*initializer)(struct page * , enum vm_type, void *) = NULL;
		switch(VM_TYPE(type)){
			case VM_ANON:
				initializer = anon_initializer;
				break;
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			default:
				free(page);
				goto err;
		}

		uninit_new (page,upage,init,type,aux,initializer);
		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		spt_lock_acquire();

		if(!spt_insert_page(spt,page)){
			free(page);
			goto err;
		}

		spt_lock_release();
		
		return true;

	}
	
	spt_lock_release();

	return true;

err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	void *start_va = pg_round_down(va);
	struct page item = { .va = start_va };
	struct hash_elem *e = hash_find(&spt->spt_hash, &item.elem);
	if (e == NULL)
		return NULL;
	
	page = hash_entry(e, struct page, elem);
	if (page == NULL)
		return NULL;
	
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page ) {
	return hash_insert (&spt->spt_hash, &page->elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	
	hash_delete (&spt->spt_hash, &page->elem);
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof *frame);
	/* TODO: Fill this function. */
	if(frame == NULL)
		return NULL;

	frame->kva = palloc_get_page(PAL_ZERO|PAL_USER);

	if(frame->kva == NULL){
		free(frame);
		PANIC("TODO");
	}

	frame->page = NULL;

	ASSERT (frame != NULL);
	
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	
	/* 1. 잘못된 주소 */
    if (addr == NULL || !is_user_vaddr (addr))
        return false;

    /* 2. 이미 매핑된 페이지의 권한 위반은 처리하지 않는다. */
    if (!not_present)
        return false;

    /* 3. 등록된 페이지인지 확인 */
	spt_lock_acquire();
    
	struct page *page = spt_find_page (spt, addr);
    
	spt_lock_release();

	if (page == NULL)
		return false;
	
	

    /* 4. 읽기 전용 페이지에 대한 쓰기 거절 */
    if (write && !page->writable)
        return false;
	
	/* 5. 다른 예외처리*/

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	/* TODO: Fill this function */
	spt_lock_acquire();

	struct page *page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;

	spt_lock_release();
	
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
// 가상 페이지의 내용을 지금 RAM에 준비하겠다. 즉, page->va에 해당하는 내용을 page->frame->kva에 준비하겠다.
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	if(frame == NULL)
		return false;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(thread_current()->pml4,page->va,frame->kva,page->writable)){
		page->frame = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}

	  /* 페이지 내용을 준비한다.
     * UNINIT 페이지라면 최초 초기화와 lazy loading이 수행된다. */
    if (!swap_in (page, frame->kva)) {
        pml4_clear_page (thread_current ()->pml4, page->va);
        page->frame = NULL;
        palloc_free_page (frame->kva);
        free (frame);
        return false;
    }

	return true;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->spt_hash, hash_key, compare_key, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

static struct lock spt_lock;

void spt_lock_init() {
	lock_init(&spt_lock);
}

void spt_lock_acquire() {
	lock_acquire(&spt_lock);
}

void spt_lock_release() {
	lock_release(&spt_lock);
}