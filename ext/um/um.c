#include <float.h>
#include "um.h"
#include "ruby/thread.h"

void um_setup(VALUE self, struct um *machine) {
  memset(machine, 0, sizeof(struct um));

  RB_OBJ_WRITE(self, &machine->self, self);

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

inline struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op) {
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

static inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (unlikely(!op)) return;

  if (!(cqe->flags & IORING_CQE_F_MORE))
    machine->pending_count--;

  // printf(
  //   ":process_cqe op %p kind %d flags %d cqe_res %d cqe_flags %d pending %d\n",
  //   op, op->kind, op->flags, cqe->res, cqe->flags, machine->pending_count
  // );

  if (op->flags & OP_F_FREE_ON_COMPLETE) {
    if (op->flags & OP_F_TRANSIENT)
      um_op_transient_remove(machine, op);

    um_op_free(machine, op);
    return;
  }

  if (unlikely((cqe->res == -ECANCELED) && (op->flags & OP_F_IGNORE_CANCELED))) return;

  op->flags |= OP_F_COMPLETED;
  if (op->flags & OP_F_TRANSIENT)
    um_op_transient_remove(machine, op);

  if (op->flags & OP_F_MULTISHOT) {
    um_op_multishot_results_push(machine, op, cqe->res, cqe->flags);
    if (op->multishot_result_count > 1)
      return;
  }
  else {
    op->result.res    = cqe->res;
    op->result.flags  = cqe->flags;
  }

  if (op->flags & OP_F_ASYNC) return;

  um_runqueue_push(machine, op);
}

// copied from liburing/queue.c
static inline int cq_ring_needs_flush(struct io_uring *ring) {
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

struct wait_for_cqe_ctx {
  struct um *machine;
  struct io_uring_cqe *cqe;
  int wait_nr;
  int result;
};

void *um_wait_for_cqe_without_gvl(void *ptr) {
  struct wait_for_cqe_ctx *ctx = ptr;
  if (ctx->machine->unsubmitted_count) {
    ctx->machine->unsubmitted_count = 0;

    // Attn: The io_uring_submit_and_wait_timeout will not return -EINTR if
    // interrupted with a signal. We can detect this by testing ctx->cqe for
    // NULL.
    //
    // https://github.com/axboe/liburing/issues/1280
    int res = io_uring_submit_and_wait_timeout(&ctx->machine->ring, &ctx->cqe, ctx->wait_nr, NULL, NULL);
    ctx->result = (res > 0 && !ctx->cqe) ? -EINTR : res;
  }
  else
    ctx->result = io_uring_wait_cqes(&ctx->machine->ring, &ctx->cqe, ctx->wait_nr, NULL, NULL);
  return NULL;
}

// Waits for the given minimum number of completion entries. The wait_nr is
// either 1 - where we wait for at least one CQE to be ready, or 0, where we
// don't wait, and just process any CQEs that already ready.
static inline void um_wait_for_and_process_ready_cqes(struct um *machine, int wait_nr) {
  struct wait_for_cqe_ctx ctx = { .machine = machine, .cqe = NULL, .wait_nr = wait_nr };
  rb_thread_call_without_gvl(um_wait_for_cqe_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);

  if (unlikely(ctx.result < 0)) {
    // the internal calls to (maybe submit) and wait for cqes may fail with:
    // -EINTR (interrupted by signal)
    // -EAGAIN (apparently can be returned when wait_nr = 0)
    // both should not raise an exception.
    switch (ctx.result) {
      case -EINTR:
      case -EAGAIN:
        // do nothing
        break;
      default:
        rb_syserr_fail(-ctx.result, strerror(-ctx.result));
    }
  }

  if (ctx.cqe) {
    um_process_cqe(machine, ctx.cqe);
    io_uring_cq_advance(&machine->ring, 1);
    um_process_ready_cqes(machine);
  }
}

inline VALUE process_runqueue_op(struct um *machine, struct um_op *op) {
  VALUE fiber = op->fiber;
  VALUE value = op->value;

  if (unlikely(op->flags & OP_F_TRANSIENT))
    um_op_free(machine, op);

  return rb_fiber_transfer(fiber, 1, &value);
}

inline VALUE um_fiber_switch(struct um *machine) {
  while (true) {
    struct um_op *op = um_runqueue_shift(machine);
    if (op) {
      // in case of a snooze, we need to prevent a situation where completions
      // are not processed because the runqueue is never empty. Theoretically,
      // we can still have a situation where multiple fibers are all doing a
      // snooze repeatedly, which can prevent completions from being processed.

      // is the op a snooze op and is this the same fiber as the current one?
      if (unlikely(op->kind == OP_SCHEDULE && op->fiber == rb_fiber_current())) {
        //  are there any pending ops (i.e. waiting for completion)?
        if (machine->pending_count > 0) {
          // if yes, process completions, get runqueue head, put original op
          // back on runqueue.
          // um_process_ready_cqes(machine);
          um_wait_for_and_process_ready_cqes(machine, 0);
          struct um_op *op2 = um_runqueue_shift(machine);
          if (likely(op2 && op2 != op)) {
            um_runqueue_push(machine, op);
            op = op2;
          }
        }
      }
      return process_runqueue_op(machine, op);
    }

    um_wait_for_and_process_ready_cqes(machine, 1);
  }
}

void um_submit_cancel_op(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_cancel64(sqe, (long long)op, 0);
}

inline void um_cancel_and_wait(struct um *machine, struct um_op *op) {
  um_submit_cancel_op(machine, op);
  while (true) {
    um_fiber_switch(machine);
    if (um_op_completed_p(op)) break;
  }
}

inline int um_check_completion(struct um *machine, struct um_op *op) {
  if (!um_op_completed_p(op)) {
    um_cancel_and_wait(machine, op);
    return 0;
  }

  um_raise_on_error_result(op->result.res);
  return 1;
}

inline VALUE um_await(struct um *machine) {
  VALUE ret = um_fiber_switch(machine);
  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

inline void um_prep_op(struct um *machine, struct um_op *op, enum op_kind kind, unsigned flags) {
  memset(op, 0, sizeof(struct um_op));
  op->kind = kind;
  op->flags = flags;

  VALUE fiber = (flags & OP_F_FREE_ON_COMPLETE) ? Qnil : rb_fiber_current();
  RB_OBJ_WRITE(machine->self, &op->fiber, fiber);
  RB_OBJ_WRITE(machine->self, &op->value, Qnil);
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);
}

inline void um_schedule(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_op_alloc(machine);
  memset(op, 0, sizeof(struct um_op));
  op->kind = OP_SCHEDULE;
  op->flags = OP_F_TRANSIENT;
  RB_OBJ_WRITE(machine->self, &op->fiber, fiber);
  RB_OBJ_WRITE(machine->self, &op->value, value);
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);
  um_runqueue_push(machine, op);
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

VALUE um_timeout_complete(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;

  if (!um_op_completed_p(ctx->op)) {
    um_submit_cancel_op(ctx->machine, ctx->op);
    ctx->op->flags |= OP_F_TRANSIENT | OP_F_IGNORE_CANCELED;
    um_op_transient_add(ctx->machine, ctx->op);
  }

  return Qnil;
}

VALUE um_timeout(struct um *machine, VALUE interval, VALUE class) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_TIMEOUT, 0);
  op->ts = um_double_to_timespec(NUM2DBL(interval));
  RB_OBJ_WRITE(machine->self, &op->fiber, rb_fiber_current());
  RB_OBJ_WRITE(machine->self, &op->value, rb_funcall(class, ID_new, 0));
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &op->ts, 0, 0);

  struct op_ctx ctx = { .machine = machine, .op = op };
  return rb_ensure(rb_yield, Qnil, um_timeout_complete, (VALUE)&ctx);
}

/*******************************************************************************
                         blocking singleshot ops
*******************************************************************************/

#define SLEEP_FOREVER_DURATION (86400*10000)

VALUE um_sleep(struct um *machine, double duration) {
  if (duration <= 0) duration = SLEEP_FOREVER_DURATION;

  struct um_op op;
  um_prep_op(machine, &op, OP_SLEEP, 0);
  op.ts = um_double_to_timespec(duration);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_timeout(sqe, &op.ts, 0, 0);
  VALUE ret = um_fiber_switch(machine);

  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    if (op.result.res != -ETIME) um_raise_on_error_result(op.result.res);
    ret = DBL2NUM(duration);
  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

inline VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, buffer_offset);
  io_uring_prep_read(sqe, fd, ptr, maxlen, -1);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op)) {
    um_update_read_buffer(machine, buffer, buffer_offset, op.result.res, op.result.flags);
    ret = INT2NUM(op.result.res);

  }
  RB_GC_GUARD(buffer);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

inline size_t um_read_raw(struct um *machine, int fd, char *buffer, int maxlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_read(sqe, fd, buffer, maxlen, -1);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op)) {
    return op.result.res;

  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return 0;
}

VALUE um_write(struct um *machine, int fd, VALUE str, int len) {
  struct um_op op;
  um_prep_op(machine, &op, OP_WRITE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  const int str_len = RSTRING_LEN(str);
  if (len > str_len) len = str_len;

  io_uring_prep_write(sqe, fd, RSTRING_PTR(str), len, -1);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RB_GC_GUARD(str);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_write_async(struct um *machine, int fd, VALUE str) {
  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_WRITE_ASYNC, OP_F_TRANSIENT | OP_F_FREE_ON_COMPLETE);
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->value, str);
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_write(sqe, fd, RSTRING_PTR(str), RSTRING_LEN(str), -1);
  um_op_transient_add(machine, op);

  return str;
}

VALUE um_close(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_CLOSE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_close(sqe, fd);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(fd);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_close_async(struct um *machine, int fd) {
  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_CLOSE_ASYNC, OP_F_FREE_ON_COMPLETE);
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->value, Qnil);
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_close(sqe, fd);

  return INT2NUM(fd);
}

VALUE um_accept(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_accept(sqe, fd, NULL, NULL, 0);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_socket(struct um *machine, int domain, int type, int protocol, uint flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SOCKET, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_socket(sqe, domain, type, protocol, flags);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_connect(struct um *machine, int fd, const struct sockaddr *addr, socklen_t addrlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_CONNECT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_connect(sqe, fd, addr, addrlen);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_send(struct um *machine, int fd, VALUE buffer, int len, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SEND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_send(sqe, fd, RSTRING_PTR(buffer), len, flags);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RB_GC_GUARD(buffer);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_send_bundle(struct um *machine, int fd, int bgid, VALUE strings) {
  um_add_strings_to_buffer_ring(machine, bgid, strings);

  struct um_op op;
  um_prep_op(machine, &op, OP_SEND_BUNDLE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

	io_uring_prep_send_bundle(sqe, fd, 0, MSG_WAITALL);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = bgid;

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_recv(struct um *machine, int fd, VALUE buffer, int maxlen, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_RECV, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, 0);
  io_uring_prep_recv(sqe, fd, ptr, maxlen, flags);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op)) {
    um_update_read_buffer(machine, buffer, 0, op.result.res, op.result.flags);
    ret = INT2NUM(op.result.res);
  }

  RB_GC_GUARD(buffer);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_bind(struct um *machine, int fd, struct sockaddr *addr, socklen_t addrlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_BIND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_bind(sqe, fd, addr, addrlen);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_listen(struct um *machine, int fd, int backlog) {
  struct um_op op;
  um_prep_op(machine, &op, OP_BIND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_listen(sqe, fd, backlog);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_getsockopt(struct um *machine, int fd, int level, int opt) {
  VALUE ret = Qnil;
  int value;

#ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op op;
  um_prep_op(machine, &op, OP_GETSOCKOPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, level, opt, &value, sizeof(value));

  ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(value);
#else
  socklen_t nvalue = sizeof(value);
  int res = getsockopt(fd, level, opt, &value, &nvalue);
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  ret = INT2NUM(value);
#endif

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_setsockopt(struct um *machine, int fd, int level, int opt, int value) {
  VALUE ret = Qnil;

#ifdef HAVE_IO_URING_PREP_CMD_SOCK
  struct um_op op;
  um_prep_op(machine, &op, OP_SETSOCKOPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, level, opt, &value, sizeof(value));

  ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);
#else
  int res = setsockopt(fd, level, opt, &value, sizeof(value));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  ret = INT2NUM(0);
#endif

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_shutdown(struct um *machine, int fd, int how) {
  VALUE ret = Qnil;

  struct um_op op;
  um_prep_op(machine, &op, OP_SHUTDOWN, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_shutdown(sqe, fd, how);

  ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_shutdown_async(struct um *machine, int fd, int how) {
  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_SHUTDOWN_ASYNC, OP_F_FREE_ON_COMPLETE);
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->value, Qnil);
  RB_OBJ_WRITE(machine->self, &op->async_op, Qnil);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_shutdown(sqe, fd, how);

  return INT2NUM(fd);
}

VALUE um_open(struct um *machine, VALUE pathname, int flags, int mode) {
  struct um_op op;
  um_prep_op(machine, &op, OP_OPEN, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_open(sqe, StringValueCStr(pathname), flags, mode);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_poll(struct um *machine, int fd, unsigned mask) {
  struct um_op op;
  um_prep_op(machine, &op, OP_POLL, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_poll_add(sqe, fd, mask);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_waitpid(struct um *machine, int pid, int options) {
  struct um_op op;
  um_prep_op(machine, &op, OP_WAITPID, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  siginfo_t infop;
  io_uring_prep_waitid(sqe, pid == 0 ? P_ALL : P_PID, pid, &infop, options, 0);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);

  return rb_ary_new_from_args(2, INT2NUM(infop.si_pid), INT2NUM(infop.si_status));
}

#define hash_set(h, sym, v) rb_hash_aset(h, ID2SYM(rb_intern(sym)), v)

VALUE statx_to_hash(struct statx *stat) {
  VALUE hash = rb_hash_new();

  hash_set(hash, "dev",         UINT2NUM(stat->stx_dev_major << 8 | stat->stx_dev_minor));
  hash_set(hash, "rdev",        UINT2NUM(stat->stx_rdev_major << 8 | stat->stx_rdev_minor));
  hash_set(hash, "blksize",     UINT2NUM(stat->stx_blksize));
  hash_set(hash, "attributes",  UINT2NUM(stat->stx_attributes));
  hash_set(hash, "nlink",       UINT2NUM(stat->stx_nlink));
  hash_set(hash, "uid",         UINT2NUM(stat->stx_uid));
  hash_set(hash, "gid",         UINT2NUM(stat->stx_gid));
  hash_set(hash, "mode",        UINT2NUM(stat->stx_mode));
  hash_set(hash, "ino",         UINT2NUM(stat->stx_ino));
  hash_set(hash, "size",        UINT2NUM(stat->stx_size));
  hash_set(hash, "blocks",      UINT2NUM(stat->stx_blocks));
  hash_set(hash, "atime",       DBL2NUM(um_timestamp_to_double(stat->stx_atime.tv_sec, stat->stx_atime.tv_nsec)));
  hash_set(hash, "btime",       DBL2NUM(um_timestamp_to_double(stat->stx_btime.tv_sec, stat->stx_btime.tv_nsec)));
  hash_set(hash, "ctime",       DBL2NUM(um_timestamp_to_double(stat->stx_ctime.tv_sec, stat->stx_ctime.tv_nsec)));
  hash_set(hash, "mtime",       DBL2NUM(um_timestamp_to_double(stat->stx_mtime.tv_sec, stat->stx_mtime.tv_nsec)));
  return hash;
}

VALUE um_statx(struct um *machine, int dirfd, VALUE path, int flags, unsigned int mask) {
  static char empty_path[] = "";

  struct um_op op;
  um_prep_op(machine, &op, OP_STATX, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  char *path_ptr = NIL_P(path) ? empty_path : StringValueCStr(path);
  struct statx stat;
  memset(&stat, 0, sizeof(stat));
  io_uring_prep_statx(sqe, dirfd, path_ptr, flags, mask, &stat);

  VALUE ret = um_fiber_switch(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);

  return statx_to_hash(&stat);
}

/*******************************************************************************
                            multishot ops
*******************************************************************************/

VALUE accept_each_start(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_multishot_accept(sqe, ctx->fd, NULL, NULL, 0);

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op)) {
      RAISE_IF_EXCEPTION(ret);
      return ret;
    }
    RB_GC_GUARD(ret);

    int more = false;
    struct um_op_result *result = &ctx->op->result;
    while (result) {
      more = (result->flags & IORING_CQE_F_MORE);
      if (result->res < 0) {
        um_op_multishot_results_clear(ctx->machine, ctx->op);
        return Qnil;
      }
      rb_yield(INT2NUM(result->res));
      result = result->next;
    }
    um_op_multishot_results_clear(ctx->machine, ctx->op);
    if (more)
      ctx->op->flags &= ~OP_F_COMPLETED;
    else
      break;
  }

  return Qnil;
}

VALUE multishot_complete(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  if (ctx->op->multishot_result_count) {
    int more = ctx->op->multishot_result_tail->flags & IORING_CQE_F_MORE;
    if (more)
      ctx->op->flags &= ~OP_F_COMPLETED;
    um_op_multishot_results_clear(ctx->machine, ctx->op);
  }
  if (!um_op_completed_p(ctx->op))
    um_cancel_and_wait(ctx->machine, ctx->op);

  if (ctx->read_buf)
    free(ctx->read_buf);

  return Qnil;
}

VALUE um_accept_each(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT_MULTISHOT, OP_F_MULTISHOT);

  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .read_buf = NULL };
  return rb_ensure(accept_each_start, (VALUE)&ctx, multishot_complete, (VALUE)&ctx);
}

int um_read_each_singleshot_loop(struct op_ctx *ctx) {
  struct buf_ring_descriptor *desc = ctx->machine->buffer_rings + ctx->bgid;
  ctx->read_maxlen = desc->buf_size;
  ctx->read_buf = malloc(desc->buf_size);
  int total = 0;

  while (1) {
    um_prep_op(ctx->machine, ctx->op, OP_READ, 0);
    struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
    io_uring_prep_read(sqe, ctx->fd, ctx->read_buf, ctx->read_maxlen, -1);

    VALUE ret = um_fiber_switch(ctx->machine);
    if (um_op_completed_p(ctx->op)) {
      um_raise_on_error_result(ctx->op->result.res);
      if (!ctx->op->result.res) return total;

      VALUE buf = rb_str_new(ctx->read_buf, ctx->op->result.res);
      total += ctx->op->result.res;
      rb_yield(buf);
      RB_GC_GUARD(buf);
    }
    else {
      RAISE_IF_EXCEPTION(ret);
      return ret;
    }
    RB_GC_GUARD(ret);
  }
}

// // returns true if more results are expected
int read_recv_each_multishot_process_result(struct op_ctx *ctx, struct um_op_result *result, int *total) {
  if (result->res == 0)
    return false;

  *total += result->res;
  VALUE buf = um_get_string_from_buffer_ring(ctx->machine, ctx->bgid, result->res, result->flags);
  rb_yield(buf);
  RB_GC_GUARD(buf);

  // TTY devices might not support multishot reads:
  // https://github.com/axboe/liburing/issues/1185. We detect this by checking
  // if the F_MORE flag is absent, then switch to single shot mode.
  if (unlikely(!(result->flags & IORING_CQE_F_MORE))) {
    *total += um_read_each_singleshot_loop(ctx);
    return false;
  }

  return true;
}

void read_recv_each_prep(struct io_uring_sqe *sqe, struct op_ctx *ctx) {
  switch (ctx->op->kind) {
    case OP_READ_MULTISHOT:
      io_uring_prep_read_multishot(sqe, ctx->fd, 0, -1, ctx->bgid);
      return;
    case OP_RECV_MULTISHOT:
      io_uring_prep_recv_multishot(sqe, ctx->fd, NULL, 0, 0);
	    sqe->buf_group = ctx->bgid;
	    sqe->flags |= IOSQE_BUFFER_SELECT;
      return;
    default:
      return;
  }
}

VALUE read_recv_each_start(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  read_recv_each_prep(sqe, ctx);
  int total = 0;

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op)) {
      RAISE_IF_EXCEPTION(ret);
      return ret;
    }
    RB_GC_GUARD(ret);

    int more = false;
    struct um_op_result *result = &ctx->op->result;
    while (result) {
      um_raise_on_error_result(result->res);

      more = (result->flags & IORING_CQE_F_MORE);
      if (!read_recv_each_multishot_process_result(ctx, result, &total))
        return Qnil;

      // rb_yield(INT2NUM(result->res));
      result = result->next;
    }
    um_op_multishot_results_clear(ctx->machine, ctx->op);
    if (more)
      ctx->op->flags &= ~OP_F_COMPLETED;
    else
      break;
  }

  return Qnil;
}

VALUE um_read_each(struct um *machine, int fd, int bgid) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ_MULTISHOT, OP_F_MULTISHOT);

  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .bgid = bgid, .read_buf = NULL };
  return rb_ensure(read_recv_each_start, (VALUE)&ctx, multishot_complete, (VALUE)&ctx);
}

VALUE um_recv_each(struct um *machine, int fd, int bgid, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_RECV_MULTISHOT, OP_F_MULTISHOT);

  struct op_ctx ctx = { .machine = machine, .op = &op, .fd = fd, .bgid = bgid, .read_buf = NULL, .flags = flags };
  return rb_ensure(read_recv_each_start, (VALUE)&ctx, multishot_complete, (VALUE)&ctx);
}

VALUE periodically_start(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_timeout(sqe, &ctx->ts, 0, IORING_TIMEOUT_MULTISHOT);

  while (true) {
    VALUE ret = um_fiber_switch(ctx->machine);
    if (!um_op_completed_p(ctx->op)) {
      RAISE_IF_EXCEPTION(ret);
      return ret;
    }
    RB_GC_GUARD(ret);

    int more = false;
    struct um_op_result *result = &ctx->op->result;
    while (result) {
      more = (result->flags & IORING_CQE_F_MORE);
      if (result->res < 0 && result->res != -ETIME) {
        um_op_multishot_results_clear(ctx->machine, ctx->op);
        return Qnil;
      }
      rb_yield(Qnil);
      result = result->next;
    }
    um_op_multishot_results_clear(ctx->machine, ctx->op);
    if (more)
      ctx->op->flags &= ~OP_F_COMPLETED;
    else
      break;
  }

  return Qnil;
}

VALUE um_periodically(struct um *machine, double interval) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SLEEP_MULTISHOT, OP_F_MULTISHOT);
  op.ts = um_double_to_timespec(interval);

  struct op_ctx ctx = { .machine = machine, .op = &op, .ts = op.ts, .read_buf = NULL };
  return rb_ensure(periodically_start, (VALUE)&ctx, multishot_complete, (VALUE)&ctx);
}
