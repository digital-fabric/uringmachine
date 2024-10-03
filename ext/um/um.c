#include "um.h"
#include "ruby/thread.h"
#include <sys/mman.h>

static inline struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe;
  sqe = io_uring_get_sqe(&machine->ring);
  if (likely(sqe)) goto done;

  rb_raise(rb_eRuntimeError, "Failed to get SQE");

  // TODO: retry getting SQE?

  // if (likely(backend->pending_sqes))
  //   io_uring_backend_immediate_submit(backend);
  // else {
  //   VALUE resume_value = backend_snooze(&backend->base);
  //   RAISE_IF_EXCEPTION(resume_value);
  // }
done:
  sqe->user_data = (long long)op;
  sqe->flags = 0;
  machine->unsubmitted_count++;
  return sqe;
}

inline void um_cleanup(struct um *machine) {
  if (!machine->ring_initialized) return;

  for (unsigned i = 0; i < machine->buffer_ring_count; i++) {
    struct buf_ring_descriptor *desc = machine->buffer_rings + i;
    io_uring_free_buf_ring(&machine->ring, desc->br, desc->buf_count, i);
    free(desc->buf_base);
  }
  machine->buffer_ring_count = 0;
  io_uring_queue_exit(&machine->ring);
  machine->ring_initialized = 0;

  um_free_linked_list(machine->runqueue_head);
  um_free_linked_list(machine->freelist_head);
}

inline void um_free_linked_list(struct um_op *op) {
  while (op) {
    struct um_op *next = op->next;
    free(op);
    op = next;
  }
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

inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (!op) return;

  switch (op->state) {
    case OP_submitted:
      op->state = OP_completed;
      op->cqe_result = cqe->res;
      op->cqe_flags = cqe->flags;
      um_runqueue_push(machine, op);
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
loop:
  op = um_runqueue_shift(machine);
  if (op) {
    VALUE resume_value = op->resume_value;
    if (op->state == OP_interrupt)
      um_op_checkin(machine, op);
    // the resume value is disregarded, we pass the fiber itself
    VALUE v = rb_fiber_transfer(op->fiber, 1, &resume_value);
    return v;
  }

  um_wait_for_and_process_ready_cqes(machine);
  goto loop;
}

static inline VALUE um_await_op(struct um *machine, struct um_op *op) {
  op->fiber = rb_fiber_current();
  VALUE v = um_fiber_switch(machine);

  int is_exception = um_value_is_exception_p(v);

  if (is_exception || op->state == OP_cancelled)
    op->state = OP_abandonned;
  else
    um_op_checkin(machine, op);
  return is_exception ? um_raise_exception(v) : v;
}

inline VALUE um_await(struct um *machine) {
  VALUE v = um_fiber_switch(machine);
  return um_value_is_exception_p(v) ? um_raise_exception(v) : v;
}

inline void um_schedule(struct um *machine, VALUE fiber, VALUE value) {
  struct um_op *op = um_op_checkout(machine);
  op->state = OP_interrupt;
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
    op->fiber = fiber;
    op->resume_value = value;
    um_runqueue_unshift(machine, op);
  }
}

inline VALUE um_sleep(struct um *machine, double duration) {
  struct um_op *op = um_op_checkout(machine);
  struct __kernel_timespec ts = um_double_to_timespec(duration);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_timeout(sqe, &ts, 0, 0);
  op->state = OP_submitted;

  return um_await_op(machine, op);
}
