#include "threads/thread.h"

#include <debug.h>
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "devices/timer.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
struct list
    sleep_list;  // timer.c 파일에서 사용 : sleep 상태인 스레드들을 담는 리스트
static struct list all_list;  // 모든 스레드를 담는 리스트(priority 재계산 용도)

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/*
* 17.14 고정소수점 : 32비트 정수를 이용해서 소수를 표현하는 방식
* 17.14 고정소수점(상위 17비트는 정수 부분, 하위 14비트는 소수 부분) 연산 매크로
* F = 2^14 : 고정소수점에서 소수점 이하는 14비트
* 정수를 고정소수점으로 바꾸려면 F를 곱하고, 고정소수점을 정수로 바꾸려면 F로
나누기
* => 고정소수점 나눗셈에서 정밀도를 유지하기 위한 스케일링 팩터

* 사용 이유 : 부동소수점 연산은 커널에서 금지되어 있으므로, 부동소수점 대신
고정소수점을 사용
* 고정소수점 사용을 통해, 반올림 오차를 줄일 수 있고 크기는 고정되며 정수로만
연산하기에 연산 속도가 빠름
*/
#define F (1 << 14)

/* F를 이용한 변환 매크로 */
// 정수 → 고정소수점
#define INT_TO_FP(n) ((n) * F)
// 고정소수점 → 정수 (내림)
#define FP_TO_INT(x) ((x) / F)
// 고정소수점 → 정수 (반올림)
#define FP_TO_INT_ROUND(x) (((x) + F / 2) / F)
// 고정소수점 × 정수
#define FP_MUL_INT(x, n) ((x) * (n))
// 고정소수점 ÷ 정수
#define FP_DIV_INT(x, n) ((x) / (n))
// 고정소수점 × 고정소수점
#define FP_MUL(x, y) (((int64_t)(x)) * (y) / F)
// 고정소수점 ÷ 고정소수점
#define FP_DIV(x, y) (((int64_t)(x)) * F / (y))
static int64_t load_avg;  // load_avg 값

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

// Function prototypes
void update_load_avg(void);
void update_recent_cpu(struct thread *t);
void update_priority(struct thread *t);
static bool priority_compare(const struct list_elem *a,
                             const struct list_elem *b, void *aux);
void calculate_and_set_priority_with_donation(struct thread *t,
                                              int new_priority);
void thread_set_priority(int new_priority);
int thread_get_priority(void);
void thread_set_nice(int nice);
int thread_get_nice(void);
int thread_get_load_avg(void);
int thread_get_recent_cpu(void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  /* Reload the temporal gdt for the kernel
   * This gdt does not include the user context.
   * The kernel will rebuild the gdt with user context, in gdt_init (). */
  struct desc_ptr gdt_ds = {.size = sizeof(gdt) - 1, .address = (uint64_t)gdt};
  lgdt(&gdt_ds);

  /* Init the globla thread context */
  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&sleep_list);
  list_init(&all_list);
  list_init(&destruction_req);

  /* MLFQS 관련 변수 초기화 */
  load_avg = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();

  /* initial_thread를 all_list에 추가 */
  list_push_back(&all_list, &initial_thread->all_elem);
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

void update_load_avg(void) {
  int ready_threads = list_size(&ready_list);
  if (thread_current() != idle_thread) {
    ready_threads += 1;
  }
  // load_avg = (59/60) * load_avg + (1/60) * ready_threads
  load_avg =
      FP_MUL(FP_DIV(INT_TO_FP(59), INT_TO_FP(60)), load_avg) +
      FP_MUL(FP_DIV(INT_TO_FP(1), INT_TO_FP(60)), INT_TO_FP(ready_threads));
}

void update_recent_cpu(struct thread *t) {
  if (t == idle_thread) return;
  // recent_cpu = (2*load_avg) / (2*load_avg + 1) * recent_cpu + nice
  int64_t load_avg_2 = FP_MUL(INT_TO_FP(2), load_avg);
  t->recent_cpu =
      FP_DIV(load_avg_2, load_avg_2 + INT_TO_FP(1)) * t->recent_cpu / F +
      INT_TO_FP(t->nice);
}

void update_priority(struct thread *t) {
  if (t == idle_thread) return;
  // priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
  int new_priority =
      FP_TO_INT_ROUND(INT_TO_FP(PRI_MAX) - FP_DIV_INT(t->recent_cpu, 4)) -
      (t->nice * 2);
  if (new_priority > PRI_MAX) new_priority = PRI_MAX;
  if (new_priority < PRI_MIN) new_priority = PRI_MIN;
  t->base_priority =
      new_priority;  // 기존 우선순위 업데이트(donation 고려 전 우선순위)

  calculate_and_set_priority_with_donation(
      t, new_priority);  // donation 고려한 우선순위 계산 및 설정
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread *t = thread_current();

  /* Update statistics. */
  if (t == idle_thread) idle_ticks++;
#ifdef USERPROG
  else if (t->pml4 != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* MLFQS가 활성화된 경우에만 매 틱마다 running thread의 recent_cpu 1 증가 */
  if (thread_mlfqs && t != idle_thread) {
    t->recent_cpu = t->recent_cpu + INT_TO_FP(1);
  }

  /* MLFQS가 활성화된 경우에만 매 초마다 모든 스레드의 recent_cpu 재계산 &
   * load_avg 재계산 */
  if (thread_mlfqs && timer_ticks() % TIMER_FREQ == 0) {
    update_load_avg();  // load_avg 재계산

    struct list_elem *elem;
    for (elem = list_begin(&all_list); elem != list_end(&all_list);
         elem = list_next(elem)) {
      struct thread *t = list_entry(elem, struct thread, all_elem);
      update_recent_cpu(t);
    }
  }

  /* MLFQS가 활성화된 경우에만 4틱마다 모든 스레드에 대해 priority 재계산 */
  if (thread_mlfqs && (timer_ticks() % 4) == 0) {
    struct list_elem *elem;
    for (elem = list_begin(&all_list); elem != list_end(&all_list);
         elem = list_next(elem)) {
      struct thread *t = list_entry(elem, struct thread, all_elem);
      update_priority(t);
    }

    // 재계산된 우선순위에 맞춰 정렬
    list_sort(&ready_list, priority_compare, NULL);

    // 현재 실행중인 스레드의 우선순위가 제일 낮아졌다면 양보
    if (!list_empty(&ready_list)) {
      struct thread *highest_priority_thread =
          list_entry(list_front(&ready_list), struct thread, elem);
      if (highest_priority_thread->priority > t->priority) {
        // 인터럽트 컨텍스트 내(timer_interrupt() => thread_tick())에서
        // thread_yield()하면 안됨 intr_yield_on_return()를 통해서 인터럽트가
        // 종료된 후에 양보하도록 설정
        intr_yield_on_return();
      }
    }
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE) intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
/*
  name : 스레드 이름(EX : "args-none", "args-single" 등)
  priority : 스레드 우선순위(PRI_MIN ~ PRI_MAX)
  function : 스레드가 처음 실행될 때 호출할 함수(= 스레드 진입점)
  aux : function에 전달할 인자(EX : NULL, "argone", "argtwo" 등)
*/
tid_t thread_create(const char *name, int priority, thread_func *function,
                    void *aux) {
  struct thread *t;
  tid_t tid;

  ASSERT(function != NULL);

  // ⭐️⭐️⭐️ 초기 실행 컨텍스트 설정 ⭐️⭐️⭐️
  /* 1. 스레드 메모리 할당 */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL) return TID_ERROR;

  /* 스레드 초기화 */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Call the kernel_thread if it scheduled.
   * Note) rdi is 1st argument, and rsi is 2nd argument.
   * 3. 논리주소 셋업 : x86-64에서 함수의 첫 번째, 두 번째 인자는 각각 rdi, rsi
   * 레지스터를 통해 전달(호출 규약)
   */

  // 👇👇👇 스레드가 처음 실행될 때 호출할 함수 설정(명령어 포인터 설정)
  t->tf.rip = (uintptr_t)
      kernel_thread;  // rip : 다음에 실행할 명령어의 주소(=함수 시작 주소)
  // 👆👆👆 시작 함수는 스레드마다 동일(rip값을 동일하게 설정)하지만, 인자에
  // 따라 각자의 함수 실행 경로를 따라가게됨

  // 👇👇👇 함수 인자 설정
  t->tf.R.rdi = (uint64_t)function;  // rdi : 첫 번째 함수 인자
  t->tf.R.rsi = (uint64_t)aux;       // rsi : 두 번째 함수 인자
  // 👆👆👆 시작 함수는 동일해도 함수 인자가 다르기 때문에, 실행 흐름은
  // 스레드마다 다름

  // 👇👇👇 세그먼트 레지스터 설정
  t->tf.ds = SEL_KDSEG;    // 데이터 세그먼트(데이터 영역의 논리주소 공간)
  t->tf.es = SEL_KDSEG;    // 확장 세그먼트(확장 영역의 논리주소 공간)
  t->tf.ss = SEL_KDSEG;    // 스택 세그먼트(스택 영역의 논리주소 공간)
  t->tf.cs = SEL_KCSEG;    // 코드 세그먼트(코드 영역의 논리주소 공간)
  t->tf.eflags = FLAG_IF;  // 플래그 레지스터
  // 👆👆👆

  /* all_list에 스레드 추가 */
  list_push_back(&all_list, &t->all_elem);

  /* Add to run queue. */
  thread_unblock(t);

  /* 새로 생성된 스레드가 현재 스레드보다 우선순위가 높으면 양보 */
  // 5. 우선순위 기반 선점
  if (t->priority > thread_current()->priority) {
    thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);
  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

void thread_preemption() {
  if (intr_context()) {
    intr_yield_on_return();
  } else {
    thread_yield();
  }
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, priority_compare, NULL);
  t->status = THREAD_READY;

  // 새로 unblocked된 스레드의 우선순위가 현재 스레드보다 높으면 선점
  if (t != idle_thread && t->priority > thread_current()->priority) {
    thread_preemption();
  }

  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *thread_current(void) {
  struct thread *t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  ASSERT(!intr_context());

#ifdef USERPROG
  process_exit();
#endif

  /* all_list에서 제거 */
  list_remove(&thread_current()->all_elem);

  /* Just set our status to dying and schedule another process.
     We will be destroyed during the call to schedule_tail(). */
  intr_disable();
  do_schedule(THREAD_DYING);
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  struct thread *curr = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (curr != idle_thread)
    list_insert_ordered(&ready_list, &curr->elem, priority_compare, NULL);
  do_schedule(THREAD_READY);
  intr_set_level(old_level);
}

// 우선순위 비교 함수(내림차순)
static bool priority_compare(const struct list_elem *a,
                             const struct list_elem *b, void *aux UNUSED) {
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);

  return thread_a->priority > thread_b->priority;
}

void calculate_and_set_priority_with_donation(struct thread *t,
                                              int new_priority) {
  // donation이 적용된, 최종 우선순위 계산
  // 나한테 기부한 스레드가 없다면, 기존 우선순위
  if (list_empty(&t->donation_list)) {
    t->priority = new_priority;
  }
  // 나한테 기부한 스레드가 있다면, 기부받은 우선순위와 기존 우선순위 중 더 높은
  // 값을 최종 우선순위 값으로 설정
  else {
    // donation_list는 우선순위 순으로 정렬된 상태(내림차순)
    struct thread *highest_donated_thread =
        list_entry(list_front(&t->donation_list), struct thread, donation_elem);

    if (highest_donated_thread->priority > new_priority) {
      t->priority = highest_donated_thread->priority;
    } else {
      t->priority = new_priority;
    }
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
  // 현재 스레드
  struct thread *current_thread = thread_current();
  current_thread->base_priority = new_priority;

  // 인터럽트 끄기
  enum intr_level old_level = intr_disable();

  // donation이 적용된, 최종 우선순위 계산
  calculate_and_set_priority_with_donation(current_thread, new_priority);

  // 현재 스레드의 우선순위가 최고가 아니라면, 즉시 CPU 양보
  // ready_list는 우선순위 순으로 정렬된 상태(내림차순)
  bool should_yield = false;
  if (!list_empty(&ready_list)) {
    struct thread *highest_priority_thread =
        list_entry(list_front(&ready_list), struct thread, elem);

    if (current_thread->priority < highest_priority_thread->priority) {
      should_yield = true;
    }
  }

  // 인터럽트 다시 켜기
  intr_set_level(old_level);

  // yield는 인터럽트 복원 후에 수행
  if (should_yield) {
    thread_yield();
  }
}

/* Returns the current thread's priority. */
int thread_get_priority(void) {
  // 인터럽트 끄기
  enum intr_level old_level = intr_disable();
  struct thread *current_thread = thread_current();  // 현재 스레드

  // 인터럽트 다시 켜기
  intr_set_level(old_level);

  // 이미 계산된(기부 상황이면 기부까지 반영된) 우선순위 값 반환
  return current_thread->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) {
  struct thread *current_thread = thread_current();
  current_thread->nice = nice;  // 1. nice 값 설정

  // 2. priority 재계산
  update_priority(current_thread);

  // 3. 필요하다면 yield
  if (!list_empty(&ready_list)) {
    struct thread *highest_priority_thread =
        list_entry(list_front(&ready_list), struct thread, elem);
    if (current_thread->priority < highest_priority_thread->priority) {
      thread_yield();
    }
  }
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  struct thread *current_thread = thread_current();
  return current_thread->nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) { return FP_TO_INT_ROUND(load_avg * 100); }

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  struct thread *current_thread = thread_current();
  return FP_TO_INT_ROUND(current_thread->recent_cpu * 100);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;

  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

       The `sti' instruction disables interrupts until the
       completion of the next instruction, so these two
       instructions are executed atomically.  This atomicity is
       important; otherwise, an interrupt could be handled
       between re-enabling interrupts and waiting for the next
       one to occur, wasting as much as one clock tick worth of
       time.

       See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
       7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func *function, void *aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread *t, const char *name, int priority) {
  // 👇👇👇 입력(인자) 검증
  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);
  // 👆👆👆

  // 👇👇👇 스레드 구조체(= 스레드 제어 블록[TCB]) 초기화 수행
  memset(t, 0, sizeof *t);  // 메모리 초기화(스레드 구조체 전체를 0으로 초기화)
                            // : 쓰레기값이 안 남도록 => 안전한 초기상태 보장
  t->status = THREAD_BLOCKED;                         // 기본 상태 설정
  strlcpy(t->name, name, sizeof t->name);             // 스레드 이름 복사
  t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);  // 스택 포인터 설정
  // 👆👆👆 메모리 레이아웃 설정

  // 👇👇👇 스레드 우선순위 및 관련 필드 초기화
  t->priority = priority;
  t->base_priority = priority;
  t->waiting_lock = NULL;
  list_init(&t->donation_list);
  t->nice = 0;
  t->recent_cpu = 0;
  // 👆👆👆

  t->magic = THREAD_MAGIC;
#ifdef USERPROG
  t->exit_status = -1;
#endif
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *next_thread_to_run(void) {
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf) {
  __asm __volatile(
      "movq %0, %%rsp\n"
      "movq 0(%%rsp),%%r15\n"
      "movq 8(%%rsp),%%r14\n"
      "movq 16(%%rsp),%%r13\n"
      "movq 24(%%rsp),%%r12\n"
      "movq 32(%%rsp),%%r11\n"
      "movq 40(%%rsp),%%r10\n"
      "movq 48(%%rsp),%%r9\n"
      "movq 56(%%rsp),%%r8\n"
      "movq 64(%%rsp),%%rsi\n"
      "movq 72(%%rsp),%%rdi\n"
      "movq 80(%%rsp),%%rbp\n"
      "movq 88(%%rsp),%%rdx\n"
      "movq 96(%%rsp),%%rcx\n"
      "movq 104(%%rsp),%%rbx\n"
      "movq 112(%%rsp),%%rax\n"
      "addq $120,%%rsp\n"
      "movw 8(%%rsp),%%ds\n"
      "movw (%%rsp),%%es\n"
      "addq $32, %%rsp\n"
      "iretq"
      :
      : "g"((uint64_t)tf)
      : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void thread_launch(struct thread *th) {
  uint64_t tf_cur = (uint64_t)&running_thread()->tf;
  uint64_t tf = (uint64_t)&th->tf;
  ASSERT(intr_get_level() == INTR_OFF);

  /* The main switching logic.
   * We first restore the whole execution context into the intr_frame
   * and then switching to the next thread by calling do_iret.
   * Note that, we SHOULD NOT use any stack from here
   * until switching is done. */
  __asm __volatile(
      /* Store registers that will be used. */
      "push %%rax\n"
      "push %%rbx\n"
      "push %%rcx\n"
      /* Fetch input once */
      "movq %0, %%rax\n"
      "movq %1, %%rcx\n"
      "movq %%r15, 0(%%rax)\n"
      "movq %%r14, 8(%%rax)\n"
      "movq %%r13, 16(%%rax)\n"
      "movq %%r12, 24(%%rax)\n"
      "movq %%r11, 32(%%rax)\n"
      "movq %%r10, 40(%%rax)\n"
      "movq %%r9, 48(%%rax)\n"
      "movq %%r8, 56(%%rax)\n"
      "movq %%rsi, 64(%%rax)\n"
      "movq %%rdi, 72(%%rax)\n"
      "movq %%rbp, 80(%%rax)\n"
      "movq %%rdx, 88(%%rax)\n"
      "pop %%rbx\n"  // Saved rcx
      "movq %%rbx, 96(%%rax)\n"
      "pop %%rbx\n"  // Saved rbx
      "movq %%rbx, 104(%%rax)\n"
      "pop %%rbx\n"  // Saved rax
      "movq %%rbx, 112(%%rax)\n"
      "addq $120, %%rax\n"
      "movw %%es, (%%rax)\n"
      "movw %%ds, 8(%%rax)\n"
      "addq $32, %%rax\n"
      "call __next\n"  // read the current rip.
      "__next:\n"
      "pop %%rbx\n"
      "addq $(out_iret -  __next), %%rbx\n"
      "movq %%rbx, 0(%%rax)\n"  // rip
      "movw %%cs, 8(%%rax)\n"   // cs
      "pushfq\n"
      "popq %%rbx\n"
      "mov %%rbx, 16(%%rax)\n"  // eflags
      "mov %%rsp, 24(%%rax)\n"  // rsp
      "movw %%ss, 32(%%rax)\n"
      "mov %%rcx, %%rdi\n"
      "call do_iret\n"
      "out_iret:\n"
      :
      : "g"(tf_cur), "g"(tf)
      : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void do_schedule(int status) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(thread_current()->status == THREAD_RUNNING);
  while (!list_empty(&destruction_req)) {
    struct thread *victim =
        list_entry(list_pop_front(&destruction_req), struct thread, elem);
    palloc_free_page(victim);
  }
  thread_current()->status = status;
  schedule();
}

static void schedule(void) {
  struct thread *curr = running_thread();
  struct thread *next = next_thread_to_run();

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(curr->status != THREAD_RUNNING);
  ASSERT(is_thread(next));
  /* Mark us as running. */
  next->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate(next);
#endif

  if (curr != next) {
    /* If the thread we switched from is dying, destroy its struct
       thread. This must happen late so that thread_exit() doesn't
       pull out the rug under itself.
       We just queuing the page free reqeust here because the page is
       currently used by the stack.
       The real destruction logic will be called at the beginning of the
       schedule(). */
    if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
      ASSERT(curr != next);
      list_push_back(&destruction_req, &curr->elem);
    }

    /* Before switching the thread, we first save the information
     * of current running. */
    thread_launch(next);
  }
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}
