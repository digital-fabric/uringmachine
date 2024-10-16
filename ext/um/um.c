#include "um.h"
#include "ruby/thread.h"

void um_setup(VALUE self, struct um *machine) {
  memset(machine, 0, sizeof(struct um));

  RB_OBJ_WRITE(self, &machine->self, self);

  unsigned prepared_limit = 4096;
  unsigned flags = 0;
  #ifdef HAVE_IORING_SETUP_SUBMIT_ALL
  flags |= IORING_SETUP_SUBMIT_ALL;
  #endif
  #ifdef HAVE_IORING_SETUP_COOP_TASKRUN
  flags |= IORING_SETUP_COOP_TASKRUN;
  #endif

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

  um_op_free_list(machine, &machine->list_idle);
  um_op_free_list(machine, &machine->list_pending);
  um_op_free_list(machine, &machine->list_scheduled);

  um_op_result_list_free(machine);
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

static inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (unlikely(!op)) return;

  // printf(
  //   ": process_cqe op %p kind %d state %d flags %d cqe_res %d cqe_flags %d\n",
  //   op, op->kind, op->state, op->flags, cqe->res, cqe->flags
  // );
  if (unlikely(op->flags & OP_F_DISCARD)) {
    // multishot ops might have multiple CQEs already queued, so we transition
    // to idle only on the last one. Otherwise, we just ignore the CQE.
    if (!(cqe->flags & IORING_CQE_F_MORE))
      um_op_state_transition(machine, op, OP_IDLE);
    return;
  }

  if (op->flags & OP_F_MULTISHOT)
    um_op_push_multishot_result(machine, op, cqe->res, cqe->flags);
  else {
    op->cqe.result = cqe->res;
    op->cqe.flags = cqe->flags;
    um_op_state_transition(machine, op, OP_SCHEDULED);
  }
}

static inline void um_wait_for_and_process_cqe(struct um *machine) {
  struct wait_for_cqe_ctx ctx = {
    .machine = machine,
    .cqe = NULL
  };

  rb_thread_call_without_gvl(um_wait_for_cqe_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);
  if (unlikely(ctx.result < 0)) {
    rb_syserr_fail(-ctx.result, strerror(-ctx.result));
  }
  io_uring_cqe_seen(&machine->ring, ctx.cqe);
  um_process_cqe(machine, ctx.cqe);
}

// copied from liburing/queue.c
static inline bool cq_ring_needs_flush(struct io_uring *ring) {
  return IO_URING_READ_ONCE(*ring->sq.kflags) & IORING_SQ_CQ_OVERFLOW;
}

static inline int um_process_ready_cqes(struct um *machine) {
  unsigned total_count = 0;
iterate:
  bool overflow_checked = false;
  struct io_uring_cqe *cqe;
  unsigned head;
  unsigned count = 0;
  io_uring_for_each_cqe(&machine->ring, head, cqe) {
    ++count;
    um_process_cqe(machine, cqe);
  }
  io_uring_cq_advance(&machine->ring, count);
  total_count += count;

  if (overflow_checked) goto done;

  if (cq_ring_needs_flush(&machine->ring)) {
    io_uring_enter(machine->ring.ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
    overflow_checked = true;
    goto iterate;
  }

done:
  return total_count;
}

static inline void um_wait_for_and_process_ready_cqes(struct um *machine) {
  um_wait_for_and_process_cqe(machine);
  um_process_ready_cqes(machine);
}

inline struct um_op *next_scheduled_op(struct um *machine) {
  struct um_op *op = um_op_list_shift(&machine->list_scheduled);
  if (op)
    um_op_state_transition(machine, op, OP_DONE);
  return op;
}

VALUE um_fiber_switch(struct um *machine) {
  struct um_op *op = 0;
  unsigned int first_iteration = 1;
loop:
  // in the case where:
  // - first time through loop
  // - there are SQEs waiting to be submitted
  // - the runqueue head references the current fiber we need to submit events
  //   and check completions without blocking.
  //
  // Bear in mind that if there's no activity, and the e.g. main fiber does a
  // snooze, this only submits new SQE's. In a test condition, for any CQEs to
  // arrive, the test code should do a second snooze.
  if (
    unlikely(
      first_iteration && machine->unsubmitted_count &&
      machine->list_scheduled.head &&
      machine->list_scheduled.head->fiber == rb_fiber_current()
    )
  ) {
    io_uring_submit(&machine->ring);
    um_process_ready_cqes(machine);
  }
  first_iteration = 0;

  op = next_scheduled_op(machine);
  if (op) {
    VALUE fiber = op->fiber;
    VALUE value = op->value;
    if (op->flags & OP_F_AUTO_CHECKIN)
      um_op_state_transition(machine, op, OP_IDLE);

    return rb_fiber_transfer(fiber, 1, &value);
  }

  um_wait_for_and_process_ready_cqes(machine);
  goto loop;
}

static inline void um_submit_cancel_op(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_cancel64(sqe, (long long)op, 0);
}

VALUE um_await_op(struct um *machine, struct um_op *op, __s32 *result, __u32 *flags) {
  RB_OBJ_WRITE(machine->self, &op->fiber, rb_fiber_current());
  
  VALUE value = um_fiber_switch(machine);
  int is_exception = um_value_is_exception_p(value);
  if (result) *result = op->cqe.result;
  if (flags) *flags = op->cqe.flags;

  if (!(op->flags & OP_F_MULTISHOT))
    um_op_state_transition(machine, op, OP_IDLE);

  if (unlikely(is_exception)) um_raise_exception(value);
  return value;
}

inline VALUE um_await(struct um *machine) {
  VALUE v = um_fiber_switch(machine);
  return um_value_is_exception_p(v) ? um_raise_exception(v) : v;
}

inline void um_schedule(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SCHEDULE);
  // TODO: directly set to COMPLETED on checkout (checkout puts it in PENDING state)
  um_op_state_transition(machine, op, OP_SCHEDULED);
  op->flags |= OP_F_AUTO_CHECKIN;
  RB_OBJ_WRITE(machine->self, &op->fiber, fiber);
  RB_OBJ_WRITE(machine->self, &op->value, value);
}

inline void um_interrupt(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_op_search_by_fiber(&machine->list_scheduled, fiber);
  if (!op)
    op = um_op_search_by_fiber(&machine->list_pending, fiber);

  if (op) {
    op->flags |= OP_F_CANCELLED;
    if (op->state == OP_PENDING)
      um_submit_cancel_op(machine, op);
    RB_OBJ_WRITE(machine->self, &op->value, value);
  }
  else {
    op = um_op_idle_checkout(machine, OP_INTERRUPT);
    // TODO: directly set to COMPLETED on checkout (checkout puts it in PENDING state)
    um_op_state_transition(machine, op, OP_SCHEDULED);
    op->flags |= OP_F_AUTO_CHECKIN;
    RB_OBJ_WRITE(machine->self, &op->fiber, fiber);
    RB_OBJ_WRITE(machine->self, &op->value, value);
  }
}

struct op_ensure_ctx {
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
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;

  if (ctx->op->state == OP_PENDING) {
    um_submit_cancel_op(ctx->machine, ctx->op);
    ctx->op->flags |= OP_F_DISCARD;
  }
  else
    um_op_state_transition(ctx->machine, ctx->op, OP_IDLE);

  return Qnil;
}

VALUE um_timeout(struct um *machine, VALUE interval, VALUE class) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = um_op_idle_checkout(machine, OP_TIMEOUT);
  op->ts = um_double_to_timespec(NUM2DBL(interval));

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);
  RB_OBJ_WRITE(machine->self, &op->fiber, rb_fiber_current());
  RB_OBJ_WRITE(machine->self, &op->value, rb_funcall(class, ID_new, 0));

  struct op_ensure_ctx ctx = { .machine = machine, .op = op };
  return rb_ensure(rb_yield, Qnil, um_timeout_ensure, (VALUE)&ctx);
}

inline VALUE um_sleep(struct um *machine, double duration) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SLEEP);
  op->ts = um_double_to_timespec(duration);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  um_await_op(machine, op, &result, NULL);
  if (result != -ETIME) um_raise_on_error_result(result);
  return Qnil;
}

inline VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset) {
  struct um_op *op = um_op_idle_checkout(machine, OP_READ);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;
  __u32 flags = 0;

  void *ptr = um_prepare_read_buffer(buffer, maxlen, buffer_offset);
  io_uring_prep_read(sqe, fd, ptr, maxlen, -1);

  um_await_op(machine, op, &result, &flags);

  um_raise_on_error_result(result);
  um_update_read_buffer(machine, buffer, buffer_offset, result, flags);
  return INT2NUM(result);
}

VALUE um_multishot_ensure(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  if (!ctx->op) return Qnil;

  if (ctx->op->aux) {
    um_op_state_transition(ctx->machine, ctx->op->aux, OP_IDLE);
    ctx->op->aux = NULL;
  }

  if (ctx->op->flags & OP_F_MULTISHOT) {
  __s32 result = 0;
  __u32 flags = 0;
    while (um_op_result_shift(ctx->machine, ctx->op, &result, &flags)) {
      if (!(flags & IORING_CQE_F_MORE)) {
        um_op_state_transition(ctx->machine, ctx->op, OP_IDLE);
        return Qnil;
      }
    }
  }

  if (ctx->op->state == OP_PENDING) {
    um_submit_cancel_op(ctx->machine, ctx->op);
    ctx->op->flags |= OP_F_DISCARD;
  }
  else
    um_op_state_transition(ctx->machine, ctx->op, OP_IDLE);
  return Qnil;
}

static inline void um_read_each_prepare_op(struct op_ensure_ctx *ctx, int singleshot_mode) {
  struct um_op *op = um_op_idle_checkout(ctx->machine, singleshot_mode ? OP_READ : OP_READ_MULTISHOT);
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, op);

  if (singleshot_mode)
    io_uring_prep_read(sqe, ctx->fd, ctx->read_buf, ctx->read_maxlen, -1);
  else {
    io_uring_prep_read_multishot(sqe, ctx->fd, 0, -1, ctx->bgid);
    op->flags |= OP_F_MULTISHOT;
  }
  ctx->op = op;
}

int um_read_each_safe_loop_singleshot(struct op_ensure_ctx *ctx, int total) {
  struct buf_ring_descriptor *desc = ctx->machine->buffer_rings + ctx->bgid;
  __s32 result = 0;
  ctx->read_maxlen = desc->buf_size;
  ctx->read_buf = malloc(desc->buf_size);

  while (1) {
    um_read_each_prepare_op(ctx, 1);
    um_await_op(ctx->machine, ctx->op, &result, NULL);
    ctx->op = NULL;
    um_raise_on_error_result(result);
    if (!result) return total;

    total += result;
    VALUE buf = rb_str_new(ctx->read_buf, result);
    rb_yield(buf);
    RB_GC_GUARD(buf);
  }
}

int read_each_multishot_process_results(struct op_ensure_ctx *ctx, int *total) {
  __s32 result = 0;
  __u32 flags = 0;
  __s32 bad_result = 0;
  int eof = 0;

  while (um_op_result_shift(ctx->machine, ctx->op, &result, &flags)) {
    if (result < 0) {
      bad_result = result;
    }
    else if (result == 0) {
      eof = 1;
    }
    else {
      *total += result;
      VALUE buf = um_get_string_from_buffer_ring(ctx->machine, ctx->bgid, result, flags);
      rb_yield(buf);
      RB_GC_GUARD(buf);
    }

    // TTY devices might not support multishot reads:
    // https://github.com/axboe/liburing/issues/1185. We detect this by checking
    // if the F_MORE flag is absent, then switch to single shot mode.
    if (!(flags & IORING_CQE_F_MORE)) {
      um_op_state_transition(ctx->machine, ctx->op, OP_IDLE);
      *total = um_read_each_safe_loop_singleshot(ctx, *total);
      return 0;
    }
    if (bad_result || eof) break;
  }

  if (bad_result)
    um_raise_on_error_result(bad_result);

  return eof ? 0 : 1;
}

VALUE um_read_each_safe_loop(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  int total = 0;

  um_read_each_prepare_op(ctx, 0);

  while (1) {
    um_await_op(ctx->machine, ctx->op, NULL, NULL);
    if (!ctx->op->aux)
      rb_raise(rb_eRuntimeError, "no associated schedule op found");
    ctx->op->aux = NULL;
    if (!ctx->op->list_results.head)
      rb_raise(rb_eRuntimeError, "no result found!\n");

    if (!read_each_multishot_process_results(ctx, &total))
      return INT2NUM(total);
  }
}

VALUE um_read_each(struct um *machine, int fd, int bgid) {
  struct op_ensure_ctx ctx = { .machine = machine, .fd = fd, .bgid = bgid, .read_buf = NULL };
  return rb_ensure(um_read_each_safe_loop, (VALUE)&ctx, um_multishot_ensure, (VALUE)&ctx);
}

VALUE um_write(struct um *machine, int fd, VALUE str, int len) {
  struct um_op *op = um_op_idle_checkout(machine, OP_WRITE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;
  __u32 flags = 0;
  const int str_len = RSTRING_LEN(str);
  if (len > str_len) len = str_len;
  struct um_buffer *buffer = um_buffer_checkout(machine, len);
  
  memcpy(buffer->ptr, RSTRING_PTR(str), len);
  io_uring_prep_write(sqe, fd, buffer->ptr, len, -1);

  um_await_op(machine, op, &result, &flags);
  um_buffer_checkin(machine, buffer);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_close(struct um *machine, int fd) {
  struct um_op *op = um_op_idle_checkout(machine, OP_CLOSE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;
  __u32 flags = 0;

  io_uring_prep_close(sqe, fd);

  um_await_op(machine, op, &result, &flags);

  um_raise_on_error_result(result);
  return INT2NUM(fd);
}

VALUE um_accept(struct um *machine, int fd) {
  struct um_op *op = um_op_idle_checkout(machine, OP_ACCEPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;
  __u32 flags = 0;

  io_uring_prep_accept(sqe, fd, NULL, NULL, 0);

  um_await_op(machine, op, &result, &flags);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_accept_each_safe_loop(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  __s32 result = 0;
  __u32 flags = 0;

  while (1) {
    um_await_op(ctx->machine, ctx->op, &result, &flags);
    if (!ctx->op->aux)
      rb_raise(rb_eRuntimeError, "no associated schedule op found");
    ctx->op->aux = NULL;
    if (!ctx->op->list_results.head) {
      // this shouldn't happen!
      rb_raise(rb_eRuntimeError, "no result found for accept_each loop");
    }

    while (um_op_result_shift(ctx->machine, ctx->op, &result, &flags)) {
      um_raise_on_error_result(result);
      if (likely(result > 0))
        rb_yield(INT2NUM(result));
      else
        return Qnil;
    }
  }
}

VALUE um_accept_each(struct um *machine, int fd) {
  struct um_op *op = um_op_idle_checkout(machine, OP_ACCEPT_MULTISHOT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
  op->flags |= OP_F_MULTISHOT;

  struct op_ensure_ctx ctx = { .machine = machine, .op = op, .read_buf = NULL };
  return rb_ensure(um_accept_each_safe_loop, (VALUE)&ctx, um_multishot_ensure, (VALUE)&ctx);
}

VALUE um_socket(struct um *machine, int domain, int type, int protocol, uint flags) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SOCKET);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_socket(sqe, domain, type, protocol, flags);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_connect(struct um *machine, int fd, const struct sockaddr *addr, socklen_t addrlen) {
  struct um_op *op = um_op_idle_checkout(machine, OP_CONNECT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_connect(sqe, fd, addr, addrlen);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_send(struct um *machine, int fd, VALUE buffer, int len, int flags) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SEND);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_send(sqe, fd, RSTRING_PTR(buffer), len, flags);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_recv(struct um *machine, int fd, VALUE buffer, int maxlen, int flags) {
  struct um_op *op = um_op_idle_checkout(machine, OP_RECV);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  void *ptr = um_prepare_read_buffer(buffer, maxlen, 0);
  io_uring_prep_recv(sqe, fd, ptr, maxlen, flags);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  um_update_read_buffer(machine, buffer, 0, result, flags);
  return INT2NUM(result);
}

static inline void recv_each_prepare_op(struct op_ensure_ctx *ctx) {
  struct um_op *op = um_op_idle_checkout(ctx->machine, OP_RECV_MULTISHOT);
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, op);
  io_uring_prep_recv_multishot(sqe, ctx->fd, 0, -1, ctx->bgid);
	sqe->buf_group = ctx->bgid;
	sqe->flags |= IOSQE_BUFFER_SELECT;
  op->flags |= OP_F_MULTISHOT;
  ctx->op = op;
}

VALUE recv_each_safe_loop(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  int total = 0;
  recv_each_prepare_op(ctx);

  while (1) {
    um_await_op(ctx->machine, ctx->op, NULL, NULL);
    if (!ctx->op->aux)
      rb_raise(rb_eRuntimeError, "no associated schedule op found");
    ctx->op->aux = NULL;
    if (!ctx->op->list_results.head)
      rb_raise(rb_eRuntimeError, "no result found!\n");

    if (!read_each_multishot_process_results(ctx, &total))
      return INT2NUM(total);
  }
}

VALUE um_recv_each(struct um *machine, int fd, int bgid, int flags) {
  struct op_ensure_ctx ctx = { .machine = machine, .fd = fd, .bgid = bgid, .read_buf = NULL, .flags = flags };
  return rb_ensure(recv_each_safe_loop, (VALUE)&ctx, um_multishot_ensure, (VALUE)&ctx);
}

VALUE um_bind(struct um *machine, int fd, struct sockaddr *addr, socklen_t addrlen) {
  struct um_op *op = um_op_idle_checkout(machine, OP_BIND);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_bind(sqe, fd, addr, addrlen);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_listen(struct um *machine, int fd, int backlog) {
  struct um_op *op = um_op_idle_checkout(machine, OP_LISTEN);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_listen(sqe, fd, backlog);
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
}

VALUE um_getsockopt(struct um *machine, int fd, int level, int opt) {
  int value;

#ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op *op = um_op_idle_checkout(machine, OP_GETSOCKOPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, level, opt, &value, sizeof(value));
  um_await_op(machine, op, &result, NULL);
  um_raise_on_error_result(result);
#else
  socklen_t nvalue = sizeof(value);
  int res = getsockopt(fd, level, opt, &value, &nvalue);
  if (res)
    rb_syserr_fail(errno, strerror(errno));
#endif

  return INT2NUM(value);
}

VALUE um_setsockopt(struct um *machine, int fd, int level, int opt, int value) {
#ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op *op = um_op_idle_checkout(machine, OP_SETSOCKOPT);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, level, opt, &value, sizeof(value));
  um_await_op(machine, op, &result, NULL);

  um_raise_on_error_result(result);
  return INT2NUM(result);
#else
  int res = setsockopt(fd, level, opt, &value, sizeof(value));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  return INT2NUM(0);
#endif
}

VALUE um_debug(struct um *machine) {
  printf("::::::::::::::::::::::::::::::\n");
  printf("idle      head %p tail %p\n", machine->list_idle.head, machine->list_idle.tail);
  printf("pending   head %p tail %p\n", machine->list_pending.head, machine->list_pending.tail);
  printf("scheduled head %p tail %p\n", machine->list_scheduled.head, machine->list_scheduled.tail);
  printf("\n");
  return machine->self;
}
