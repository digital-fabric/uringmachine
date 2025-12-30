#include "um.h"
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <unistd.h>
#include <ruby/thread.h>

#define FUTEX2_SIZE_U32		        0x02
#define SIDECAR_THREAD_STACK_SIZE PTHREAD_STACK_MIN

#define RAISE_ON_ERR(ret) if (ret) rb_syserr_fail(errno, strerror(errno))

static inline int futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

struct futex_wait_ctx {
  uint32_t *futexp;
  uint32_t oldval;
};

void *futex_wait_without_gvl(void *ptr) {
  struct futex_wait_ctx *ctx = ptr;
  futex(ctx->futexp, FUTEX_WAIT, ctx->oldval, NULL, NULL, 0);
  return NULL;
}

static inline void xchg_futex_wait(uint32_t *futexp, uint32_t oldval, uint32_t newval) {
  struct futex_wait_ctx ctx = { futexp, oldval };
  while (1) {
    if (atomic_compare_exchange_strong(futexp, &newval, oldval))
      break;

    rb_thread_call_without_gvl(futex_wait_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);
    // int ret = futex(futexp, FUTEX_WAIT, oldval, NULL, NULL, 0);

  }
}

static inline void xchg_futex_wake(uint32_t *futexp, uint32_t oldval, uint32_t newval) {
  while (1) {
    if (atomic_compare_exchange_strong(futexp, &newval, oldval))
      break;

    usleep(1);
  }

  futex(futexp, FUTEX_WAKE, 1, NULL, NULL, 0);
}

inline void um_sidecar_signal_wait(struct um *machine) {
  // wait for machine->sidecar_signal to equal 1, then reset it to 0
  xchg_futex_wait(machine->sidecar_signal, 0, 1);
}

inline void um_sidecar_signal_wake(struct um *machine) {
  // busy-wait for machine->sidecar_signal to equal 0, then set it to 1 and wakeup futex waiter
  xchg_futex_wake(machine->sidecar_signal, 1, 0);
}

static void *sidecar_start(void *arg) {
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
  struct um *machine = arg;
  while (1) {
    int ret = io_uring_enter2(machine->ring.enter_ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    if (!ret) {
      um_sidecar_signal_wake(machine);
  }
  }
  return NULL;
}

void um_sidecar_setup(struct um *machine) {
  if (machine->sidecar_thread) return;

  int                 ret;
  pthread_attr_t      attr;

  ret = pthread_attr_init(&attr);
  RAISE_ON_ERR(ret);

  ret = pthread_attr_setstacksize(&attr, SIDECAR_THREAD_STACK_SIZE);
  RAISE_ON_ERR(ret);

  sigset_t sigmask;
  sigemptyset(&sigmask);
  ret = pthread_attr_setsigmask_np(&attr, &sigmask);
  RAISE_ON_ERR(ret);

  ret = pthread_create(&machine->sidecar_thread, &attr, sidecar_start, machine);
  RAISE_ON_ERR(ret);

  ret = pthread_attr_destroy(&attr);
  RAISE_ON_ERR(ret);
}


void um_sidecar_teardown(struct um *machine) {
  if (machine->sidecar_thread) {
    pthread_cancel(machine->sidecar_thread);
    pthread_join(machine->sidecar_thread, NULL);

    machine->sidecar_thread = 0;
  }
}
