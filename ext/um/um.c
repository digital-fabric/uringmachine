#include "um.h"
#include "ruby/thread.h"
#include <sys/mman.h>

void um_setup(struct um *machine) {
  machine->ring_initialized = 0;
  machine->unsubmitted_count = 0;
  machine->buffer_ring_count = 0;
  machine->pending_count = 0;
  machine->runqueue_head = NULL;
  machine->runqueue_tail = NULL;
  machine->op_freelist = NULL;
  machine->result_freelist = NULL;

  unsigned prepared_limit = 4096;
  int flags = 0;
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

  um_free_op_linked_list(machine, machine->op_freelist);
  um_free_op_linked_list(machine, machine->runqueue_head);
}

static inline struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op) {
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

inline void um_handle_submitted_op_cqe_single(struct um *machine, struct um_op *op, struct io_uring_cqe *cqe) {
  op->cqe_result = cqe->res;
  op->cqe_flags = cqe->flags;
  op->state = OP_completed;
  um_runqueue_push(machine, op);
}

inline void um_handle_submitted_op_cqe_multi(struct um *machine, struct um_op *op, struct io_uring_cqe *cqe) {
  if (!op->results_head) {
    struct um_op *op2 = um_op_checkout(machine);
    op2->state = OP_schedule;
    op2->fiber = op->fiber;
    op2->resume_value = Qnil;
    um_runqueue_push(machine, op2);
  }
  um_op_result_push(machine, op, cqe->res, cqe->flags);

  if (!(cqe->flags & IORING_CQE_F_MORE))
    op->state = OP_completed;
}

inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (unlikely(!op)) return;

  switch (op->state) {
    case OP_submitted:
      if (unlikely(cqe->res == -ECANCELED)) {
        um_op_checkin(machine, op);
        break;
      }
      if (!op->is_multishot)
        um_handle_submitted_op_cqe_single(machine, op, cqe);
      else
        um_handle_submitted_op_cqe_multi(machine, op, cqe);
      break;
    case OP_abandonned:
      // op has been abandonned by the I/O method, so we need to cleanup (check
      // the op in to the free list).
      um_op_checkin(machine, op);
    default:
      // TODO: invalid state, should raise!
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

inline VALUE um_fiber_switch(struct um *machine) {
  struct um_op *op = 0;
  unsigned int first_iteration = 1;
loop:
  // in the case where:
  // - first time through loop
  // - there are SQEs waiting to be submitted
  // - the runqueue head references the current fiber
  // we need to submit events and check completions without blocking
  if (
    unlikely(
      first_iteration && machine->unsubmitted_count && 
      machine->runqueue_head &&
      machine->runqueue_head->fiber == rb_fiber_current()
    )
  ) {
    io_uring_submit(&machine->ring);
    um_process_ready_cqes(machine);
  }
  first_iteration = 0;

  op = um_runqueue_shift(machine);
  if (op) {
    VALUE resume_value = op->resume_value;
    if (op->state == OP_schedule) {
      um_op_checkin(machine, op);
    }
    // the resume value is disregarded, we pass the fiber itself
    VALUE v = rb_fiber_transfer(op->fiber, 1, &resume_value);
    return v;
  }

  um_wait_for_and_process_ready_cqes(machine);
  goto loop;
}

static inline void um_cancel_op(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_cancel64(sqe, (long long)op, 0);
}

static inline VALUE um_await_op(struct um *machine, struct um_op *op, int *result, int *flags) {
  op->fiber = rb_fiber_current();
  VALUE v = um_fiber_switch(machine);
  int is_exception = um_value_is_exception_p(v);

  if (unlikely(is_exception && op->state == OP_submitted)) {
    um_cancel_op(machine, op);
    op->state = OP_abandonned;
  }
  else {
    // We copy over the CQE result and flags, since the op is immediately
    // checked in.
    if (result) *result = op->cqe_result;
    if (flags) *flags = op->cqe_flags;
    if (!op->is_multishot)
      um_op_checkin(machine, op);
  }

  if (unlikely(is_exception)) um_raise_exception(v);
  return v;
}

inline VALUE um_await(struct um *machine) {
  VALUE v = um_fiber_switch(machine);
  return um_value_is_exception_p(v) ? um_raise_exception(v) : v;
}

inline void um_schedule(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_op_checkout(machine);
  op->state = OP_schedule;
  op->fiber = fiber;
  op->resume_value = value;
  um_runqueue_push(machine, op);
}

inline void um_interrupt(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_runqueue_find_by_fiber(machine, fiber);
  if (op) {
    op->state = OP_cancelled;
    op->resume_value = value;
  }
  else {
    op = um_op_checkout(machine);
    op->state = OP_schedule;
    op->fiber = fiber;
    op->resume_value = value;
    um_runqueue_unshift(machine, op);
  }
}

struct op_ensure_ctx {
  struct um *machine;
  struct um_op *op;
  int bgid;
};

VALUE um_timeout_ensure(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;

  if (ctx->op->state == OP_submitted) {
    // A CQE has not yet been received, we cancel the timeout and abandon the op
    // (it will be checked in upon receiving the -ECANCELED CQE)
    um_cancel_op(ctx->machine, ctx->op);
    ctx->op->state == OP_abandonned;
  }
  else {
    // completed, so can be checked in
    um_op_checkin(ctx->machine, ctx->op);
  }
  return Qnil;
}

VALUE um_timeout(struct um *machine, VALUE interval, VALUE class) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  struct um_op *op = um_op_checkout(machine);
  struct __kernel_timespec ts = um_double_to_timespec(NUM2DBL(interval));

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &ts, 0, 0);
  op->state = OP_submitted;
  op->fiber = rb_fiber_current();
  op->resume_value = rb_funcall(class, ID_new, 0);

  struct op_ensure_ctx ctx = { .machine = machine, .op = op };
  return rb_ensure(rb_yield, Qnil, um_timeout_ensure, (VALUE)&ctx);
}

inline VALUE um_sleep(struct um *machine, double duration) {
  struct um_op *op = um_op_checkout(machine);
  struct __kernel_timespec ts = um_double_to_timespec(duration);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  int result = 0;

  io_uring_prep_timeout(sqe, &ts, 0, 0);
  op->state = OP_submitted;

  return um_await_op(machine, op, &result, NULL);
}

inline VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset) {
  struct um_op *op = um_op_checkout(machine);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  int result = 0;
  int flags = 0;

  void *ptr = um_prepare_read_buffer(buffer, maxlen, buffer_offset);
  io_uring_prep_read(sqe, fd, ptr, maxlen, -1);
  op->state = OP_submitted;

  um_await_op(machine, op, &result, &flags);

  um_raise_on_system_error(result);
  um_update_read_buffer(machine, buffer, buffer_offset, result, flags);
  return INT2FIX(result);
}

VALUE um_read_each_ensure(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  switch (ctx->op->state) {
    case OP_submitted:
      um_cancel_op(ctx->machine, ctx->op);
      break;
    case OP_completed:
      um_op_checkin(ctx->machine, ctx->op);
      break;
    default:  
  }
  return Qnil;
}

VALUE um_read_each_safe_loop(VALUE arg) {
  struct op_ensure_ctx *ctx = (struct op_ensure_ctx *)arg;
  int result = 0;
  int flags = 0;
  int total = 0;

  while (1) {
    um_await_op(ctx->machine, ctx->op, NULL, NULL);
    if (!ctx->op->results_head) {
      // TODO: raise, this shouldn't happen
      printf("no result found!\n");
    }
    while (um_op_result_shift(ctx->machine, ctx->op, &result, &flags)) {
      if (likely(result > 0)) {
        total += result;
        VALUE buf = get_string_from_buffer_ring(ctx->machine, ctx->bgid, result, flags);
        rb_yield(buf);
      }
      else
        return INT2FIX(total);
    }
  }
}

VALUE um_read_each(struct um *machine, int fd, int bgid) {
  struct um_op *op = um_op_checkout(machine);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);

  op->is_multishot = 1;
  io_uring_prep_read_multishot(sqe, fd, 0, -1, bgid);
  op->state = OP_submitted;

  struct op_ensure_ctx ctx = { .machine = machine, .op = op, .bgid = bgid };
  return rb_ensure(um_read_each_safe_loop, (VALUE)&ctx, um_read_each_ensure, (VALUE)&ctx);
}
