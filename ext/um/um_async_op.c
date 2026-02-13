#include "um.h"
#include <stdlib.h>

VALUE um_prep_timeout(struct um *machine, double interval) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = um_op_acquire(machine);
  um_prep_op(machine, op, OP_TIMEOUT, 2, OP_F_ASYNC | OP_F_TRANSIENT);
  op->ts = um_double_to_timespec(interval);

  VALUE obj = rb_funcall(cAsyncOp, ID_new, 0);
  um_async_op_set(obj, machine, op);

  RB_OBJ_WRITE(machine->self, &op->async_op, obj);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  return obj;
}

VALUE um_async_op_await(struct um_async_op *async_op) {
  if (OP_CQE_DONE_P(async_op->op)) return async_op->op->value;

  RB_OBJ_WRITE(async_op->machine->self, &async_op->op->fiber, rb_fiber_current());
  um_op_transient_remove(async_op->machine, async_op->op);
  async_op->op->flags &= ~(OP_F_ASYNC | OP_F_TRANSIENT);

  VALUE ret = um_switch(async_op->machine);

  um_verify_op_completion(async_op->machine, async_op->op, true);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return INT2NUM(async_op->op->result.res);
}

void um_async_op_cancel(struct um_async_op *async_op) {
  um_cancel_op(async_op->machine, async_op->op);
}
