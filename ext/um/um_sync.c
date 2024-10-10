#include "um.h"
#include <stdatomic.h>
#include <linux/futex.h>

struct sync_ctx {
  struct um *machine;
  uint32_t *futex;
};

#define FUTEX2_SIZE_U32		0x02

void um_futex_wait(struct um *machine, uint32_t *futex, uint32_t expect) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SYNCHRONIZE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  // submit futex_wait
  io_uring_prep_futex_wait(
    sqe, (uint32_t *)futex, expect, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );
  um_await_op(machine, op, &result, NULL);
  if (result != -EAGAIN)
    um_raise_on_error_result(result);
}

void um_futex_wake(struct um *machine, uint32_t *futex, uint32_t num_waiters) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SYNCHRONIZE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  // submit futex_wait
  io_uring_prep_futex_wake(
    sqe, (uint32_t *)futex, num_waiters, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );
  um_await_op(machine, op, &result, NULL);
  um_raise_on_error_result(result);
}

#define LOCKED 1
#define UNLOCKED 0

void um_mutex_init(uint32_t *futex) {
  *futex = UNLOCKED;
}

void um_mutex_lock(struct um *machine, uint32_t *futex) {
  while (*futex == LOCKED) {
    um_futex_wait(machine, futex, LOCKED);
  }
  *futex = LOCKED;
}

void um_mutex_unlock(struct um *machine, uint32_t *futex) {
  *futex = UNLOCKED;
  // Wake up 1 waiting fiber
  um_futex_wake(machine, futex, 1);
}

VALUE synchronize_begin(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_lock(ctx->machine, ctx->futex);
  return rb_yield(Qnil);
}

VALUE synchronize_ensure(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_unlock(ctx->machine, ctx->futex);
  return Qnil;
}

VALUE um_mutex_synchronize(struct um *machine, uint32_t *futex) {
  struct sync_ctx ctx = { .machine = machine, .futex = futex };
  return rb_ensure(synchronize_begin, (VALUE)&ctx, synchronize_ensure, (VALUE)&ctx);
}
