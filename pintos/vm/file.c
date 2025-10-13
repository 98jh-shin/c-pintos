/* file.c: Implementation of memory backed file object (mmaped object). */

#include "filesys/file.h"  // file_read_at, file_write_at

#include "threads/synch.h"     // filesys_lock
#include "threads/thread.h"    // thread_current(), pml4
#include "threads/vaddr.h"     // PGSIZE
#include "userprog/process.h"  // struct vm_load_arg
#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &file_ops;

  struct file_page *file_page = &page->file;

  struct vm_load_arg *aux = (struct vm_load_arg *)page->uninit.aux;
  file_page->file = aux->file;
  file_page->offset = aux->ofs;
  file_page->page_read_bytes = aux->read_bytes;

  return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page UNUSED = &page->file;

  int read = file_read_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->offset);
  memset(page->frame->kva + read, 0, PGSIZE - read);
  return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;

  struct frame *frame = page->frame;

  if (pml4_is_dirty(thread_current()->pml4, page->va)) {
    file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->offset);
    pml4_set_dirty(thread_current()->pml4, page->va, false);
  }
  page->frame->page = NULL;
  page->frame = NULL;
  pml4_clear_page(thread_current()->pml4, page->va);
  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;

  if (pml4_is_dirty(thread_current()->pml4, page->va)) {
    file_write_at(file_page->file, page->va, file_page->page_read_bytes, file_page->offset);
    pml4_set_dirty(thread_current()->pml4, page->va, false);
  }

  if (page->frame) {
    list_remove(&page->frame->frame_elem);
    page->frame->page = NULL;
    page->frame = NULL;
  }

  pml4_clear_page(thread_current()->pml4, page->va);
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
        struct file *file, off_t offset) {
}
static void *find_free_address(struct thread *t, size_t length) {
  size_t page_count = (length + PGSIZE - 1) / PGSIZE;

/* Do the munmap */
void do_munmap(void *addr) {
}