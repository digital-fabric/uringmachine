VALUE um_sleep(struct um *machine, double duration) {
  struct um_op *op = um_op_acquire(machine);
  op->fiber = rb_fiber_current();
  op->ts = um_double_to_timespec(duration);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  VALUE ret = um_switch(machine);
  if (um_verify_op_completion(machine, op)) ret = DBL2NUM(duration);
  um_op_release(machine, op);

  RAISE_IF_EXCEPTION(ret);
  return ret;
}

VALUE um_switch(struct um *machine) {
  while (true) {
    struct um_op *op = um_runqueue_shift(machine);
    if (op) return rb_fiber_transfer(op->fiber, 1, &op->value);

    um_wait_for_and_process_ready_cqes(machine);
  }
}

void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (op) {
    op->result.res    = cqe->res;
    op->result.flags  = cqe->flags;
    um_runqueue_push(machine, op);
  }
}
