#include "um.h"
#include <stdlib.h>

VALUE um_prep_timeout(struct um *machine, double interval) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = malloc(sizeof(struct um_op));
  um_prep_op(machine, op, OP_TIMEOUT, OP_F_TRANSIENT | OP_F_ASYNC);
  op->ts = um_double_to_timespec(interval);

  VALUE obj = rb_funcall(cAsyncOp, rb_intern_const("new"), 0);
  um_async_op_set(obj, machine, op);

  RB_OBJ_WRITE(machine->self, &op->async_op, obj);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  um_op_transient_add(machine, op);

  return obj;
}

VALUE um_async_op_await(struct um_async_op *async_op) {
  RB_OBJ_WRITE(async_op->machine->self, &async_op->op->fiber, rb_fiber_current());
  async_op->op->flags &= ~OP_F_ASYNC;

  VALUE ret = um_fiber_switch(async_op->machine);
  if (!um_op_completed_p(async_op->op))
    um_cancel_and_wait(async_op->machine, async_op->op);

  raise_if_exception(ret);
  return INT2NUM(async_op->op->result.res);
}

void um_async_op_cancel(struct um_async_op *async_op) {
  um_submit_cancel_op(async_op->machine, async_op->op);
}
