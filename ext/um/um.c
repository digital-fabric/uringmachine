#include "um.h"
#include "ruby/thread.h"

void um_setup(VALUE self, struct um *machine) {
  memset(machine, 0, sizeof(struct um));

  RB_OBJ_WRITE(self, &machine->self, self);
  RB_OBJ_WRITE(self, &machine->poll_fiber, Qnil);

  unsigned prepared_limit = 4096;
  unsigned flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN;

  while (1) {
    int ret = io_uring_queue_init(prepared_limit, &machine->ring, flags);
    if (likely(!ret)) break;

    // if ENOMEM is returned, try with half as much entries
    if (unlikely(ret == -ENOMEM && prepared_limit > 64))
      prepared_limit = prepared_limit / 2;
    else
      rb_syserr_fail(-ret, strerror(-ret));
  }
  machine->ring_initialized = 1;
}

inline void um_teardown(struct um *machine) {
  if (!machine->ring_initialized) return;

  for (unsigned i = 0; i < machine->buffer_ring_count; i++) {
    struct buf_ring_descriptor *desc = machine->buffer_rings + i;
    io_uring_free_buf_ring(&machine->ring, desc->br, desc->buf_count, i);
    free(desc->buf_base);
  }
  machine->buffer_ring_count = 0;
  io_uring_queue_exit(&machine->ring);
  machine->ring_initialized = 0;

  um_free_buffer_linked_list(machine);
}

struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe;
  sqe = io_uring_get_sqe(&machine->ring);
  if (likely(sqe)) goto done;

  rb_raise(rb_eRuntimeError, "Failed to get SQE");

  // TODO: retry getting SQE?

  // if (likely(backend->pending_sqes))
  //   io_uring_um_immediate_submit(backend);
  // else {
  //   VALUE resume_value = um_snooze(&backend->base);
  //   RAISE_IF_EXCEPTION(resume_value);
  // }
done:
  sqe->user_data = (long long)op;
  sqe->flags = 0;
  machine->unsubmitted_count++;
  if (op) machine->pending_count++;
  return sqe;
}

struct wait_for_cqe_ctx {
  struct um *machine;
  struct io_uring_cqe *cqe;
  int result;
};

void *um_wait_for_cqe_without_gvl(void *ptr) {
  struct wait_for_cqe_ctx *ctx = ptr;
  if (ctx->machine->unsubmitted_count) {
    ctx->machine->unsubmitted_count = 0;
    ctx->result = io_uring_submit_and_wait_timeout(&ctx->machine->ring, &ctx->cqe, 1, NULL, NULL);
  }
  else
    ctx->result = io_uring_wait_cqe(&ctx->machine->ring, &ctx->cqe);
  return NULL;
}

static inline VALUE um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (unlikely(!op)) return Qnil;

  if (!(cqe->flags & IORING_CQE_F_MORE))
    machine->pending_count--;

  int more = (cqe->flags & IORING_CQE_F_MORE);
  printf(
    ": process_cqe op %p kind %d flags %d cqe_res %d cqe_flags %d pending %d more %d\n",
    op, op->kind, op->flags, cqe->res, cqe->flags, machine->pending_count, more
  );
  INSPECT("  fiber", op->fiber);
  INSPECT("  value", op->value);

  VALUE fiber   = op->fiber;
  VALUE value   = op->value;
  int inhibit_fiber_transfer = unlikely((cqe->res == -ECANCELED) && (op->flags & OP_F_IGNORE_CANCELED));

  if (unlikely(op->flags & OP_F_TRANSIENT)) {
    um_op_transient_remove(machine, op);
    free(op);
  }
  else {
    op->cqe_res   = cqe->res;
    op->cqe_flags = cqe->flags;
    op->flags |= OP_F_COMPLETED;
  }

  return inhibit_fiber_transfer ? Qnil : rb_fiber_transfer(fiber, 1, &value);
}

static inline VALUE um_wait_for_and_process_cqe(struct um *machine) {
  struct wait_for_cqe_ctx ctx = {
    .machine = machine,
    .cqe = NULL
  };

  rb_thread_call_without_gvl(um_wait_for_cqe_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);
  if (unlikely(ctx.result < 0)) {
    rb_syserr_fail(-ctx.result, strerror(-ctx.result));
  }
  io_uring_cqe_seen(&machine->ring, ctx.cqe);
  return um_process_cqe(machine, ctx.cqe);
}

// copied from liburing/queue.c
static inline bool cq_ring_needs_flush(struct io_uring *ring) {
  return IO_URING_READ_ONCE(*ring->sq.kflags) & IORING_SQ_CQ_OVERFLOW;
}

// static inline unsigned um_process_ready_cqes(struct um *machine, VALUE *ret) {
//   unsigned total_count = 0;
// iterate:
//   bool overflow_checked = false;
//   struct io_uring_cqe *cqe;
//   unsigned head;
//   unsigned count = 0;
//   io_uring_for_each_cqe(&machine->ring, head, cqe) {
//     ++count;
//     *ret = um_process_cqe(machine, cqe);
//   }
//   io_uring_cq_advance(&machine->ring, count);
//   total_count += count;

//   if (overflow_checked) goto done;

//   if (cq_ring_needs_flush(&machine->ring)) {
//     io_uring_enter(machine->ring.ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
//     overflow_checked = true;
//     goto iterate;
//   }

// done:
//   return total_count;
// }

// static inline VALUE um_wait_for_and_process_ready_cqes(struct um *machine) {
//   VALUE ret = um_wait_for_and_process_cqe(machine);
//   um_process_ready_cqes(machine, &ret);
//   RB_GC_GUARD(ret);
//   return ret;
// }

// returns true if a cqe was processed
static inline int um_process_next_ready_cqe(struct um *machine, VALUE *ret) {
iterate:
  bool overflow_checked = false;
  struct io_uring_cqe *cqe;
  unsigned head;
  unsigned count = 0;
  io_uring_for_each_cqe(&machine->ring, head, cqe) {
    ++count;
    *ret = um_process_cqe(machine, cqe);
    break;
  }
  io_uring_cq_advance(&machine->ring, count);
  if (count > 0) return true;

  if (overflow_checked) goto done;

  if (cq_ring_needs_flush(&machine->ring)) {
    io_uring_enter(machine->ring.ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
    overflow_checked = true;
    goto iterate;
  }

done:
  return false;
}

// VALUE um_poll(struct um *machine) {
//   RB_OBJ_WRITE(machine->self, &machine->poll_fiber, rb_fiber_current());
//   INSPECT("um_poll >>", machine->poll_fiber);
// // await:
//   VALUE ret = um_wait_for_and_process_ready_cqes(machine);
//   // INSPECT("um_poll ret", ret);
//   // printf("unsubmitted_count: %d\n", machine->unsubmitted_count);
//   // if (machine->unsubmitted_count && (ret == Qnil))
//   //   goto await;
//   RB_OBJ_WRITE(machine->self, &machine->poll_fiber, Qnil);

//   INSPECT("um_poll <<", ret);
//   return ret;
// }

VALUE um_fiber_switch(struct um *machine) {
  VALUE ret = Qnil;
  if (!um_process_next_ready_cqe(machine, &ret))
    ret = um_wait_for_and_process_cqe(machine);
  
  return ret;

  // static VALUE nil = Qnil;
  // if (machine->poll_fiber != Qnil)
  //   return rb_fiber_transfer(machine->poll_fiber, 1, &nil);
  // else
  //   return um_poll(machine);
}

static inline void um_submit_cancel_op(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_cancel64(sqe, (long long)op, 0);
}

void um_cancel_and_wait(struct um *machine, struct um_op *op) {
  um_submit_cancel_op(machine, op);
  while (true) {
    um_fiber_switch(machine);
    if (um_op_completed_p(op)) break;
  }
}

inline VALUE um_await(struct um *machine) {
  VALUE v = um_fiber_switch(machine);
  return raise_if_exception(v);
}

inline void um_schedule(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = malloc(sizeof(struct um_op));
  memset(op, 0, sizeof(struct um_op));
  op->kind = OP_SCHEDULE;
  op->flags = OP_F_TRANSIENT;
  RB_OBJ_WRITE(machine->self, &op->fiber, fiber);
  RB_OBJ_WRITE(machine->self, &op->value, value);
  um_op_transient_add(machine, op);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_nop(sqe);
}

inline void um_interrupt(struct um *machine, VALUE fiber, VALUE value) {
  um_schedule(machine, fiber, value);
}

struct op_ctx {
  struct um *machine;
  struct um_op *op;
  int fd;
  int bgid;

  void *read_buf;
  int read_maxlen;
  struct __kernel_timespec ts;
  int flags;
};

VALUE um_timeout_ensure(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;

  if (!um_op_completed_p(ctx->op)) {
    um_submit_cancel_op(ctx->machine, ctx->op);
    ctx->op->flags |= OP_F_TRANSIENT | OP_F_IGNORE_CANCELED;
    um_op_transient_add(ctx->machine, ctx->op);
  }

  return Qnil;
}

void um_prep_op(struct um *machine, struct um_op *op, enum op_kind kind) {
  memset(op, 0, sizeof(struct um_op));
  op->kind = kind;
  RB_OBJ_WRITE(machine->self, &op->fiber, rb_fiber_current());
  op->value = Qnil;
}

VALUE um_timeout(struct um *machine, VALUE interval, VALUE class) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = malloc(sizeof(struct um_op));
  um_prep_op(machine, op, OP_TIMEOUT);
  op->ts = um_double_to_timespec(NUM2DBL(interval));
  RB_OBJ_WRITE(machine->self, &op->fiber, rb_fiber_current());
  RB_OBJ_WRITE(machine->self, &op->value, rb_funcall(class, ID_new, 0));

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  struct op_ctx ctx = { .machine = machine, .op = op };
  return rb_ensure(rb_yield, Qnil, um_timeout_ensure, (VALUE)&ctx);
}

VALUE um_sleep(struct um *machine, double duration) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SLEEP);
  op.ts = um_double_to_timespec(duration);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_timeout(sqe, &op.ts, 0, 0);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    if (op.cqe_res != -ETIME) um_raise_on_error_result(op.cqe_res);
    ret = DBL2NUM(duration);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

inline VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, buffer_offset);
  io_uring_prep_read(sqe, fd, ptr, maxlen, -1);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    um_update_read_buffer(machine, buffer, buffer_offset, op.cqe_res, op.cqe_flags);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(buffer);
  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_write(struct um *machine, int fd, VALUE str, int len) {
  struct um_op op;
  um_prep_op(machine, &op, OP_WRITE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  const int str_len = RSTRING_LEN(str);
  if (len > str_len) len = str_len;
  struct um_buffer *buffer = um_buffer_checkout(machine, len);  
  memcpy(buffer->ptr, RSTRING_PTR(str), len);
  io_uring_prep_write(sqe, fd, buffer->ptr, len, -1);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  um_buffer_checkin(machine, buffer);

  RB_GC_GUARD(str);
  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_close(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_CLOSE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_close(sqe, fd);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(fd);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_accept(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_accept(sqe, fd, NULL, NULL, 0);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_socket(struct um *machine, int domain, int type, int protocol, uint flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SOCKET);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_socket(sqe, domain, type, protocol, flags);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_connect(struct um *machine, int fd, const struct sockaddr *addr, socklen_t addrlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_CONNECT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_connect(sqe, fd, addr, addrlen);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_send(struct um *machine, int fd, VALUE buffer, int len, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SEND);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_send(sqe, fd, RSTRING_PTR(buffer), len, flags);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(buffer);
  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_recv(struct um *machine, int fd, VALUE buffer, int maxlen, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_RECV);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, 0);
  io_uring_prep_recv(sqe, fd, ptr, maxlen, flags);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    um_update_read_buffer(machine, buffer, 0, op.cqe_res, op.cqe_flags);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(buffer);
  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_bind(struct um *machine, int fd, struct sockaddr *addr, socklen_t addrlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_BIND);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_bind(sqe, fd, addr, addrlen);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_listen(struct um *machine, int fd, int backlog) {
  struct um_op op;
  um_prep_op(machine, &op, OP_BIND);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_listen(sqe, fd, backlog);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_getsockopt(struct um *machine, int fd, int level, int opt) {
  VALUE ret = Qnil;
  int value;

  #ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op op;
  um_prep_op(machine, &op, OP_GETSOCKOPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, level, opt, &value, sizeof(value));
  ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(value);
  }
  #else
  socklen_t nvalue = sizeof(value);
  int res = getsockopt(fd, level, opt, &value, &nvalue);
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  ret = INT2NUM(value);
  #endif

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}

VALUE um_setsockopt(struct um *machine, int fd, int level, int opt, int value) {
  VALUE ret = Qnil;

  #ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op op;
  um_prep_op(machine, &op, OP_GETSOCKOPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, level, opt, &value, sizeof(value));
  ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    um_raise_on_error_result(op.cqe_res);
    ret = INT2NUM(op.cqe_res);
  }
  #else
  int res = setsockopt(fd, level, opt, &value, sizeof(value));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  ret = INT2NUM(0);
  #endif

  RB_GC_GUARD(ret);
  return raise_if_exception(ret);
}





























VALUE accept_each_begin(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_multishot_accept(sqe, ctx->fd, NULL, NULL, 0);

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op))
      return raise_if_exception(ret);
    else {
      int more = (ctx->op->cqe_flags & IORING_CQE_F_MORE);
      if (more) ctx->op->flags &= ~ OP_F_COMPLETED;
      um_raise_on_error_result(ctx->op->cqe_res);
      rb_yield(INT2NUM(ctx->op->cqe_res));
      if (!more) break;
    }
  }

  return Qnil;
}

VALUE multishot_ensure(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  if (!um_op_completed_p(ctx->op))
    um_cancel_and_wait(ctx->machine, ctx->op);

  if (ctx->read_buf)
    free(ctx->read_buf);

  return Qnil;
}

VALUE um_accept_each(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT_MULTISHOT);
  
  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .read_buf = NULL };
  return rb_ensure(accept_each_begin, (VALUE)&ctx, multishot_ensure, (VALUE)&ctx);
}

int um_read_each_singleshot_loop(struct op_ctx *ctx) {
  printf("!!!um_read_each_singleshot_loop\n");

  struct buf_ring_descriptor *desc = ctx->machine->buffer_rings + ctx->bgid;
  ctx->read_maxlen = desc->buf_size;
  ctx->read_buf = malloc(desc->buf_size);
  int total = 0;

  while (1) {
    um_prep_op(ctx->machine, ctx->op, OP_READ);
    struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
    io_uring_prep_read(sqe, ctx->fd, ctx->read_buf, ctx->read_maxlen, -1);
    VALUE ret = um_fiber_switch(ctx->machine);

    if (um_op_completed_p(ctx->op)) {
      um_raise_on_error_result(ctx->op->cqe_res);
      if (!ctx->op->cqe_res) return total;

      VALUE buf = rb_str_new(ctx->read_buf, ctx->op->cqe_res);
      total += ctx->op->cqe_res;
      rb_yield(buf);
      RB_GC_GUARD(buf);
    }
    else
      return raise_if_exception(ret);
  }
}

// returns true if more results are expected
int read_each_multishot_process_result(struct op_ctx *ctx, int *total) {
  printf("!!! read_each_multishot_process_result op %p res %d flags %d\n", ctx->op, ctx->op->cqe_res, ctx->op->cqe_flags);
  if (ctx->op->cqe_res == 0)
    return false;

  *total += ctx->op->cqe_res;
  VALUE buf = um_get_string_from_buffer_ring(ctx->machine, ctx->bgid, ctx->op->cqe_res, ctx->op->cqe_flags);
  rb_yield(buf);
  RB_GC_GUARD(buf);

  // TTY devices might not support multishot reads:
  // https://github.com/axboe/liburing/issues/1185. We detect this by checking
  // if the F_MORE flag is absent, then switch to single shot mode.

  if (unlikely(!(ctx->op->cqe_flags & IORING_CQE_F_MORE))) {
    *total += um_read_each_singleshot_loop(ctx);
    return false;
  }

  return true;
}

VALUE read_each_begin(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;

  printf("read_each_begin op %p fd %d bgid %d\n", ctx->op, ctx->fd, ctx->bgid);
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_read_multishot(sqe, ctx->fd, 0, -1, ctx->bgid);
	sqe->flags |= IOSQE_BUFFER_SELECT;
  int total = 0;

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op))
      return raise_if_exception(ret);
    else {
      int more = (ctx->op->cqe_flags & IORING_CQE_F_MORE);
      if (more) ctx->op->flags &= ~ OP_F_COMPLETED;
      um_raise_on_error_result(ctx->op->cqe_res);
      if (!read_each_multishot_process_result(ctx, &total))
        return INT2NUM(total);
    }
  }

  return Qnil;
}

VALUE um_read_each(struct um *machine, int fd, int bgid) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ_MULTISHOT);

  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .bgid = bgid, .read_buf = NULL };
  return rb_ensure(read_each_begin, (VALUE)&ctx, multishot_ensure, (VALUE)&ctx);
}

VALUE recv_each_begin(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;

  printf("recv_each_begin op %p fd %d bgid %d\n", ctx->op, ctx->fd, ctx->bgid);
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_recv_multishot(sqe, ctx->fd, NULL, 0, 0);
	sqe->buf_group = ctx->bgid;
	sqe->flags |= IOSQE_BUFFER_SELECT;
  int total = 0;

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op))
      return raise_if_exception(ret);
    else {
      int more = (ctx->op->cqe_flags & IORING_CQE_F_MORE);
      if (more) ctx->op->flags &= ~ OP_F_COMPLETED;
      um_raise_on_error_result(ctx->op->cqe_res);
      if (!read_each_multishot_process_result(ctx, &total))
        return INT2NUM(total);
    }
  }

  return Qnil;
}

VALUE um_recv_each(struct um *machine, int fd, int bgid, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_RECV_MULTISHOT);

  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .bgid = bgid, .read_buf = NULL, .flags = flags };
  return rb_ensure(recv_each_begin, (VALUE)&ctx, multishot_ensure, (VALUE)&ctx);
}
