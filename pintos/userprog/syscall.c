#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "filesys/off_t.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/thread.h"
#include "userprog/gdt.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
int write(int fd, const void *buffer, unsigned size);

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

void syscall_init(void) {
  write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG)
                                                               << 32);
  write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

  /* The interrupt service rountine should not serve any interrupts
   * until the syscall_entry swaps the userland stack to the kernel
   * mode stack. Therefore, we masked the FLAG_FL. */
  write_msr(MSR_SYSCALL_MASK,
            FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED) {
  /* 시스템 콜 번호에 따라 적절한 핸들러 호출 */
  int syscall_number =
      f->R.rax;  // rax 레지스터에 시스템콜 번호가 저장되어 있음

  switch (syscall_number) {
    case SYS_HALT:
      power_off();
      break;
    case SYS_EXIT:
      int status = (int)f->R.rdi;
      struct thread *curr = thread_current();
#ifdef USERPROG
      curr->exit_status = status;
#endif
      thread_exit();
      break;
    case SYS_WRITE:
      f->R.rax =
          write((int)f->R.rdi, (const void *)f->R.rsi, (unsigned)f->R.rdx);
      break;
    default:
      printf("system call 오류 : 알 수 없는 시스템콜 번호 %d\n",
             syscall_number);
      thread_exit();
  }
}

int write(int fd, const void *buffer, unsigned size) {
  /* fd가 1이면 콘솔에 출력 : putbuf() 함수를 1번만 호출해서 전체 버퍼를 출력 */
  if (fd == 1) {
    if ((size == 0) || (buffer == NULL)) return 0;  // 잘못된 경우 0 반환

    putbuf(buffer, size);
    return size;  // 출력한 바이트 수 반환
  }

  /* ⭐️⭐️⭐️ 파일 쓰기 : 파일 크기 확장 불가 ⭐️⭐️⭐️ */
  // struct file *file =
  //     process_get_file(fd); /* 파일 디스크립터로부터 파일 구조체 얻기 */
  // if (file == NULL || buffer == NULL || size == 0) return 0;

  // // 파일 끝까지 최대한 많이 쓰기
  // off_t length = file_length(file);  // 파일 전체 크기
  // off_t file_pos = file_tell(file);  // 현재 파일 포인터 위치
  // unsigned max_write_size = 0;       // 실제로 쓸 수 있는 최대 바이트 수

  // // 파일 끝까지 쓸 수 있는 바이트 수 계산
  // if (file_pos < length) {
  //   // 파일 포인터가 파일 끝보다 앞에 있는 경우 : 남는 공간만큼 사용 가능
  //   max_write_size = length - file_pos;

  //   if (size < max_write_size)
  //     // 남는 공간보다 요청 크기가 더 작으면 : 요청 크기만큼만 사용
  //     max_write_size = size;
  // } else {
  //   max_write_size = 0;
  // }

  // // 실제 쓰기 및 반환 : max_write_size만큼만 사용
  // unsigned bytes_written = file_write(file, buffer, max_write_size);
  // return bytes_written;
}
