#include "um.h"
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <unistd.h>

#define FUTEX2_SIZE_U32		        0x02
#define SIDECAR_THREAD_STACK_SIZE 8192

#define RAISE_ON_ERR(ret) if (ret) rb_syserr_fail(errno, strerror(errno))

static void *sidecar_start(void *arg) {
  struct um *machine = arg;
  while (1) {

  }
  return NULL;
}

void um_sidecar_setup(struct um *machine) {
  int                 ret;
  pthread_attr_t      attr;

  machine->sidecar_signal = aligned_alloc(4, sizeof(uint32_t));
  memset(machine->sidecar_signal, 0, sizeof(uint32_t));

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
}


void um_sidecar_teardown(struct um *machine) {
  int ret;

  ret = pthread_cancel(machine->sidecar_thread);
  RAISE_ON_ERR(ret);

  ret = pthread_join(machine->sidecar_thread, NULL);
  RAISE_ON_ERR(ret);

  free(machine->sidecar_signal);
}

static inline int futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

static void xchg_futex_wait(uint32_t *futexp, uint32_t oldval, uint32_t newval) {
  while (1) {

    /* Is the futex available? */
    if (atomic_compare_exchange_strong(futexp, &newval, oldval))
      break;      /* Yes */

    /* Futex is not available; wait. */

    int ret = futex(futexp, FUTEX_WAIT, oldval, NULL, NULL, 0);
    if (ret == -1 && errno != EAGAIN) rb_syserr_fail(errno, strerror(errno));
  }
}

static void xchg_futex_wake(uint32_t *futexp, uint32_t oldval, uint32_t newval) {
  while (1) {

    /* Is the futex available? */
    if (atomic_compare_exchange_strong(futexp, &newval, oldval))
      break;      /* Yes */

    usleep(1);
  }

  futex(futexp, FUTEX_WAKE, newval, NULL, NULL, 0);
}

void um_sidecar_signal_wait(struct um *machine) {
  // wait for machine->sidecar_signal to equal 1, then reset it to 0
  xchg_futex_wait(machine->sidecar_signal, 0, 1);
}

void um_sidecar_signal_wake(struct um *machine) {
  // busy-wait for machine->sidecar_signal to equal 0, then set it to 1 and wakeup futex waiter
  xchg_futex_wake(machine->sidecar_signal, 1, 0);
}
