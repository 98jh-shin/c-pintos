/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "vm/inspect.h"

static struct list frame_table;  // 구조체 추가
struct lock frame_lock;
struct list_elem *next = NULL;
static bool is_valid_stack_access(void *addr, const uintptr_t rsp);

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
  vm_anon_init();
  vm_file_init();
#ifdef EFILESYS /* For project 4 */
  pagecache_init();
#endif
  register_inspect_intr();
  /* DO NOT MODIFY UPPER LINES. */
  /* TODO: Your code goes here. */
  list_init(&frame_table);  // 구조체 초기화
  lock_init(&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
    default:
      return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/*
 * vm_alloc_page_with_initializer()
 *
 * 새로운 가상 페이지를 생성하고, 초기화 함수를 연결한 뒤
 * Supplemental Page Table(SPT)에 삽입하는 함수.
 *
 * 주요 동작:
 *  1. 요청된 가상 주소(upage)가 이미 SPT에 존재하는지 확인
 *  2. 존재하지 않으면 struct page 동적 할당
 *  3. 페이지 타입(VM_ANON, VM_FILE)에 따라 적절한 초기화 함수 선택
 *  4. uninit_new()를 호출하여 "미할당(uninitialized)" 페이지로 생성
 *     - 실제 물리 프레임은 아직 매핑하지 않음
 *     - 초기화 함수 포인터와 aux를 보관하여 나중에 lazy load 시 사용
 *  5. writable 여부 저장
 *  6. 생성한 page를 SPT에 삽입
 *
 * 성공 시 true 반환, 실패 시 false 반환
 */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux) {
  ASSERT(VM_TYPE(type) != VM_UNINIT)  // 타입이 UNINIT 자체로 들어오면 안 됨

  struct supplemental_page_table *spt = &thread_current()->spt;

  /* 1. upage가 이미 SPT에 등록되어 있는지 확인 */
  if (spt_find_page(spt, upage) == NULL) {
    /* 2. 새로운 page 구조체 동적 할당 */
    struct page *page = malloc(sizeof(struct page));
    if (!page) goto err;  // 메모리 부족 → 실패 처리

    /* 3. 타입에 따라 실제 초기화 함수 선택 */
    typedef bool (*initializer_by_type)(struct page *, enum vm_type, void *);
    initializer_by_type initializer = NULL;

    switch (VM_TYPE(type)) {
      case VM_ANON:  // 익명 페이지
        initializer = anon_initializer;
        break;
      case VM_FILE:  // 파일 매핑 페이지
        initializer = file_backed_initializer;
        break;
    }

    /* 4. "uninit" 페이지로 생성
     *    - 실제 내용은 아직 로드되지 않음 (lazy load 예정)
     *    - 나중에 page fault 발생 시 init()가 불려서 데이터 로드 */
    uninit_new(page, upage, init, type, aux, initializer);

    /* 5. 페이지 속성 기록 (쓰기 가능 여부) */
    page->writable = writable;

    /* 6. SPT에 삽입 → 성공 시 true 반환 */
    return spt_insert_page(spt, page);
  }

err:
  return false;  // 이미 존재하거나 메모리 부족 → 실패
}

/*
 * spt_find_page()
 *
 * - 주어진 가상 주소 va에 대응되는 페이지를 SPT에서 검색한다.
 * - 내부적으로 hash_find()를 이용하여 spt_hash에서 조회한다.
 * - 해시 키로 사용할 page 구조체를 임시로 생성하여
 *   같은 va를 가진 hash_elem을 찾는다.
 * - 찾으면 해당 struct page를 반환, 없으면 NULL 반환.
 */
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
  /* 1. 비교용 임시 page 구조체 생성 */
  struct page page;

  /* 2. va를 페이지 크기(PGSIZE) 단위로 내림 정렬 → 페이지 기준 주소 */
  page.va = pg_round_down(va);

  /* 3. 해시 테이블에서 같은 va를 가진 page 탐색 */
  struct hash_elem *e = hash_find(&spt->spt_hash, &page.hash_elem);

  /* 4. 찾았으면 struct page*로 변환해서 반환, 없으면 NULL */
  return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/*
 * spt_insert_page()
 *
 * - 새로운 page를 SPT에 삽입한다.
 * - 삽입 시 동일한 va가 이미 존재하면 실패(false) 반환.
 * - 성공적으로 삽입 시 true 반환.
 */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED,
                     struct page *page UNUSED) {
  /* hash_insert는 삽입 실패 시 기존 요소의 포인터를 반환,
   * 성공 시 NULL을 반환한다.
   * 따라서 NULL이 아니면 이미 존재하는 것 → false */
  return hash_insert(&spt->spt_hash, &page->hash_elem) ? false : true;
}

/*
 * spt_remove_page()
 *
 * - 주어진 page를 SPT에서 제거한다.
 * - 단순히 해시에서 제거하는 게 아니라,
 *   page에 연결된 자원(프레임, 메모리 등)을 해제해야 한다.
 * - vm_dealloc_page()를 호출해 파괴(destroy) 및 free까지 진행.
 */
void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
  hash_delete(&spt->spt_hash, &page->hash_elem);
  vm_dealloc_page(page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  struct frame *victim = NULL;

  lock_acquire(&frame_lock);
  for (next = list_begin(&frame_table); next != list_end(&frame_table); next = list_next(next)) {
    victim = list_entry(next, struct frame, frame_elem);
    if (pml4_is_accessed(thread_current()->pml4, victim->page->va))
      pml4_set_accessed(thread_current()->pml4, victim->page->va, false);
    else {
      lock_release(&frame_lock);
      return victim;
    }
  }
  lock_release(&frame_lock);

  return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim = vm_get_victim();
  if (victim->page)
    swap_out(victim->page);
  return victim;

  return NULL;
}

/*
 * vm_get_frame()
 *
 * - 사용자 풀(User Pool)에서 새로운 물리 프레임을 할당한다.
 * - 만약 할당할 수 있는 물리 프레임이 없으면, 페이지 교체(eviction)를 통해
 *   프레임을 확보한다.
 * - 최종적으로 유효한 프레임을 반환한다.
 */
static struct frame *vm_get_frame(void) {
  struct frame *frame = NULL;

  /* 1. 프레임 구조체 자체를 동적 할당한다.
   *    - frame 구조체는 커널 영역에 존재하며,
   *      kva(물리 주소)와 page(연결된 페이지)를 저장한다. */
  frame = (struct frame *)malloc(sizeof(struct frame));
  ASSERT(frame != NULL);

  /* 2. 사용자 풀에서 실제 물리 메모리 1페이지를 얻는다.
   *    - PAL_USER: 사용자 영역에서 할당
   *    - PAL_ZERO: 페이지를 0으로 초기화 */
  frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);

  /* 3. 물리 페이지를 얻지 못했을 경우 → 프레임이 가득 찼다는 뜻
   *    - 이때는 교체 정책(eviction policy)을 통해
   *      victim frame을 골라 swap out 한 뒤 프레임을 회수해야 한다. */
  if (frame->kva == NULL)
    frame = vm_evict_frame();
  else
    /* 4. 정상적으로 프레임을 확보했다면
     *    frame_table (글로벌 프레임 리스트)에 추가한다.
     *    - 이 리스트는 교체 알고리즘에서 victim 선택할 때 사용됨 */
    list_push_back(&frame_table, &frame->frame_elem);

  /* 5. 새로 생성한 프레임은 아직 어떤 페이지와도 연결되지 않았다.
   *    따라서 초기값으로 NULL을 설정. */
  frame->page = NULL;

  /* 6. 방금 만든 프레임은 페이지와 연결되지 않은 상태여야 한다는 검증 */
  ASSERT(frame->page == NULL);

  /* 7. 유효한 프레임 반환 */
  return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {
  if (!page->accessible)
    return false;

  void *kva = page->frame->kva;

  page->frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);

  if (page->frame->kva == NULL)
    page->frame = vm_evict_frame();

  memcpy(page->frame->kva, kva, PGSIZE);

  if (!pml4_set_page(thread_current()->pml4, page->va, page->frame->kva, page->accessible))
    return false;

  return true;
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present) {
  struct supplemental_page_table *spt = &thread_current()->spt;
  struct page *page = NULL;
  // ============================================
  // Step 1: 주소 정렬 및 기본 검증
  // ============================================

  // 1: addr을 페이지 단위로 정렬
  // 페이지 시작 주소로 정렬
  void *page_addr = pg_round_down(addr);

  // 2: User 영역인지 확인 : 보안
  if (!is_user_vaddr(page_addr)) {
    return false;
  }

  // ============================================
  // Step 2: SPT에서 페이지 찾기
  // ============================================

  page = spt_find_page(spt, page_addr);

  if (page != NULL) {
    return vm_do_claim_page(page);
  }

  // ============================================
  // Step 3: Stack Growth 확인
  // ============================================
  // rsp 근처인지, stack 영역인지, 1MB 제한을 넘었는지 (1 << 20 = 1MB)
  if (is_valid_stack_access(page_addr, f->rsp)) {
    vm_stack_growth(page_addr);
    return true;
  }

  // ============================================
  // Step 4: Invalid Access 처리
  // ============================================
  return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/*
 * vm_claim_page()
 *
 * - 주어진 가상 주소 va 에 해당하는 페이지를 SPT에서 찾아서
 *   실제 물리 프레임을 할당(Claim)하고 매핑까지 완료하는 함수.
 * - 즉, "이 주소의 페이지를 실제 메모리에 올려라"라는 요청을 수행.
 */
bool vm_claim_page(void *va UNUSED) {
  struct page *page = NULL;

  /* SPT에서 va에 해당하는 페이지 구조체를 검색한다.
   * 만약 존재하지 않으면 (즉, 아직 관리되지 않는 주소라면) 실패 처리. */
  page = spt_find_page(&thread_current()->spt, va);

  if (page == NULL) return false;

  /* 페이지를 실제 물리 프레임에 할당하고 매핑하는 작업 진행 */
  return vm_do_claim_page(page);
}

/*
 * vm_do_claim_page()
 *
 * - 실제로 페이지를 물리 프레임과 연결하고,
 *   MMU(Page Table)에 매핑을 설정하는 함수.
 */
static bool vm_do_claim_page(struct page *page) {
  /* 1. 사용자 풀에서 새 물리 프레임을 가져온다.
   *    만약 여유 공간이 없다면, swap out 으로 victim 교체가 일어날 수도 있음. */
  struct frame *frame = vm_get_frame();

  /* 2. 양방향 연결 설정
   *    - frame이 어떤 page에 속하는지 기록
   *    - page가 어떤 frame을 사용하는지 기록 */
  frame->page = page;
  page->frame = frame;

  /* 3. 페이지 테이블에 (page->va → frame->kva) 매핑 추가
   *    - pml4_set_page: 현재 스레드의 pml4(Page Map Level 4, top-level PT)에
   *      가상주소와 물리주소를 매핑한다.
   *    - writable 플래그에 따라 쓰기 가능 여부를 설정한다.
   *    - 실패하면 false 반환. */
  if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) return false;

  /* 4. 페이지 타입에 맞게 실제 데이터를 메모리에 적재 (swap_in 호출)
   *    - 예: Lazy load의 경우 파일에서 읽어오기
   *    - 익명 페이지(anon)의 경우 swap disk에서 가져오기
   *    - 성공하면 true, 실패하면 false */
  return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED) {
  hash_init(&spt->spt_hash, page_hash, page_less, NULL);  // spt 초기화
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst, struct supplemental_page_table *src) {
  struct hash_iterator i;
  struct hash *src_hash = &src->spt_hash;
  struct hash *dst_hash = &dst->spt_hash;

  hash_first(&i, src_hash);
  while (hash_next(&i)) {
    struct page *src_page = hash_entry(hash_cur(&i), struct page, hash_elem);

    enum vm_type type = src_page->operations->type;
    if (type == VM_UNINIT) {  // 초기화되지 않은 페이지(VM_UNINIT)인 경우
      struct uninit_page *uninit_page = &src_page->uninit;
      struct file_loader *file_loader = (struct file_loader *)uninit_page->aux;

      // 새로운 파일 로더(new_file_loader)를 할당하고 기존의 파일 로더 정보를 복사
      struct file_loader *new_file_loader = malloc(sizeof(struct file_loader));
      memcpy(new_file_loader, uninit_page->aux, sizeof(struct file_loader));
      new_file_loader->file = file_duplicate(file_loader->file);  // 파일을 복제하여 새로운 파일 포인터를 생성

      // 초기화할 페이지에 신규 파일 로더를 이용하여 초기화할 페이지 할당
      vm_alloc_page_with_initializer(uninit_page->type, src_page->va, true, uninit_page->init, new_file_loader);
      vm_claim_page(src_page->va);                                    // 페이지를 소유하고 있는 스레드의 페이지 테이블(pml4)에 페이지 등록
    } else {                                                          // 초기화된 페이지인 경우
      vm_alloc_page(src_page->operations->type, src_page->va, true);  // 페이지 할당
      vm_claim_page(src_page->va);                                    // 페이지를 소유하고 있는 스레드의 페이지 테이블(pml4)에 페이지 등록
      memcpy(src_page->va, src_page->frame->kva, PGSIZE);             // 페이지의 가상 주소에 초기화된 데이터 복사
    }
  }

  return true;
}

// hash_clear 두 번째 인자로 입력될 콜백 함수
void hash_action_destroy(struct hash_elem *hash_elem_, void *aux) {
  struct page *page = hash_entry(hash_elem_, struct page, hash_elem);

  if (page != NULL) {
    vm_dealloc_page(page);
  }
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
  hash_clear(&spt->spt_hash, hash_action_destroy);
}

/*
 * Hash function for supplemental page table (SPT).
 *
 * - 해시 테이블에서 특정 page 를 저장할 때 사용할 해시 값 계산 함수.
 * - struct page 의 가상 주소 (page->va)를 기반으로 해시를 생성한다.
 * - hash_bytes(): 주어진 메모리 블록을 바이트 단위로 해싱하는 Pintos 유틸 함수.
 * - 결국 동일한 가상 주소를 가진 page 는 동일한 해시 값으로 매핑됨.
 */
uint64_t page_hash(const struct hash_elem *e, void *aux UNUSED) {
  struct page *page = hash_entry(e, struct page, hash_elem);
  return hash_bytes(&page->va, sizeof(*page->va));
}

/*
 * Comparison function for supplemental page table (SPT).
 *
 * - 해시 테이블에서 같은 버킷에 충돌이 발생했을 때 원소 간 정렬을 위해 사용된다.
 * - 두 struct page 의 가상 주소 (va)를 비교하여 작은 쪽이 앞으로 오도록 정렬한다.
 * - 결국 해시 테이블 내부에서 page 들은 va 기준 오름차순으로 정리됨.
 */
bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
  struct page *page_a = hash_entry(a, struct page, hash_elem);
  struct page *page_b = hash_entry(b, struct page, hash_elem);

  return page_a->va < page_b->va;
}

static bool is_valid_stack_access(void *addr, const uintptr_t rsp) {
  uintptr_t fault_addr = (uintptr_t)addr;

  // stack 영역 확인
  if (fault_addr >= USER_STACK) return false;
  // rsp 근처 확인
  if (fault_addr < rsp - 32) return false;
  // 1MB 제한 확인
  if (USER_STACK - fault_addr > (1 << 20)) return false;

  return true;
}

// frame의 존재는 함수 호출자에서 확인
void free_frame(struct frame *frame) {
  list_remove(&frame->frame_elem);
  pml4_clear_page(thread_current()->pml4, frame->page->va);
  palloc_free_page(frame->kva);
  free(frame);
}