#include "um.h"
#include <float.h>
#include <ruby/thread.h>
#include <assert.h>
#include <poll.h>
#include <liburing/io_uring.h>

#define DEFAULT_SIZE 4096

inline void prepare_io_uring_params(struct io_uring_params *params, uint sqpoll_timeout_msec) {
  memset(params, 0, sizeof(struct io_uring_params));
  params->flags = IORING_SETUP_SUBMIT_ALL;
  if (sqpoll_timeout_msec) {
    params->flags |= IORING_SETUP_SQPOLL;
    params->sq_thread_idle = sqpoll_timeout_msec;
  }
  else
    params->flags |= IORING_SETUP_COOP_TASKRUN;
}

void um_setup(VALUE self, struct um *machine, uint size, uint sqpoll_timeout_msec, int sidecar_mode) {
  memset(machine, 0, sizeof(struct um));

  RB_OBJ_WRITE(self, &machine->self, self);
  RB_OBJ_WRITE(self, &machine->pending_fibers, rb_set_new());

  machine->size = (size > 0) ? size : DEFAULT_SIZE;
  machine->sqpoll_mode = !!sqpoll_timeout_msec;

  // sidecar handling
  machine->sidecar_mode = sidecar_mode;
  machine->sidecar_signal = aligned_alloc(4, sizeof(uint32_t));
  memset(machine->sidecar_signal, 0, sizeof(uint32_t));

  struct io_uring_params params;
  prepare_io_uring_params(&params, sqpoll_timeout_msec);
  int ret = io_uring_queue_init_params(machine->size, &machine->ring, &params);
  if (ret) rb_syserr_fail(-ret, strerror(-ret));
  machine->ring_initialized = 1;

  if (machine->sidecar_mode) um_sidecar_setup(machine);
}

inline void um_teardown(struct um *machine) {
  if (!machine->ring_initialized) return;

  if (machine->sidecar_mode) um_sidecar_teardown(machine);
  if (machine->sidecar_signal) free(machine->sidecar_signal);

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
  DEBUG_PRINTF("-> %p um_get_sqe: op %p kind=%s unsubmitted=%d pending=%d total=%lu\n",
    &machine->ring, op, um_op_kind_name(op ? op->kind : OP_UNDEFINED),
    machine->metrics.ops_unsubmitted, machine->metrics.ops_pending, machine->metrics.total_ops
  );

  struct io_uring_sqe *sqe;
  sqe = io_uring_get_sqe(&machine->ring);
  if (likely(sqe)) goto done;

  um_raise_internal_error("Submission queue full. Consider raising the machine size.");

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
  machine->metrics.ops_unsubmitted++;
  if (op) {
    machine->metrics.ops_pending++;
    machine->metrics.total_ops++;
  }
  return sqe;
}

struct um_submit_ctx {
  struct um *machine;
  int result;
};

// adapted from liburing/src/queue.c
static inline bool sq_ring_needs_enter(struct um *machine) {
  if (machine->sqpoll_mode) {
	  io_uring_smp_mb();
  	if (unlikely(IO_URING_READ_ONCE(*machine->ring.sq.kflags) & IORING_SQ_NEED_WAKEUP))
	  	return true;
  }
  return true;
}

void *um_submit_without_gvl(void *ptr) {
  struct um_submit_ctx *ctx = ptr;
  ctx->result = io_uring_submit(&ctx->machine->ring);
  return NULL;
}

inline uint um_submit(struct um *machine) {
  DEBUG_PRINTF("-> %p um_submit: unsubmitted=%d pending=%d total=%lu\n",
    &machine->ring, machine->metrics.ops_unsubmitted, machine->metrics.ops_pending,
    machine->metrics.total_ops
  );
  if (!machine->metrics.ops_unsubmitted) {
    DEBUG_PRINTF("<- %p um_submit: no unsubmitted SQEs, early return\n", &machine->ring);
    return 0;
  }

  struct um_submit_ctx ctx = { .machine = machine };
  if (sq_ring_needs_enter(machine))
    rb_thread_call_without_gvl(um_submit_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);
  else
    ctx.result = io_uring_submit(&machine->ring);

  DEBUG_PRINTF("<- %p um_submit: result=%d\n", &machine->ring, ctx.result);

  if (ctx.result < 0)
    rb_syserr_fail(-ctx.result, strerror(-ctx.result));

  machine->metrics.ops_unsubmitted = 0;
  return ctx.result;
}

static inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;
  if (DEBUG) {
    if (op) {
      DEBUG_PRINTF("<- %p um_process_cqe: op %p kind %s flags %d cqe_res %d cqe_flags %d pending %d\n",
        &machine->ring, op, um_op_kind_name(op->kind), op->flags, cqe->res, cqe->flags, machine->metrics.ops_pending
      );
    }
    else {
      DEBUG_PRINTF("<- %p um_process_cqe: op NULL cqe_res %d cqe_flags %d pending %d\n",
        &machine->ring, cqe->res, cqe->flags, machine->metrics.ops_pending
      );
    }
  }
  if (unlikely(!op)) return;

  if (!(cqe->flags & IORING_CQE_F_MORE))
    machine->metrics.ops_pending--;

  if (op->flags & OP_F_FREE_ON_COMPLETE) {
    if (op->flags & OP_F_TRANSIENT)
      um_op_transient_remove(machine, op);

    um_op_free(machine, op);
    return;
  }

  op->flags |= OP_F_COMPLETED;
  if (unlikely((cqe->res == -ECANCELED) && (op->flags & OP_F_IGNORE_CANCELED))) return;
  if (unlikely(op->flags & OP_F_CANCELED)) return;

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
  DEBUG_PRINTF("-> %p um_process_ready_cqes: unsubmitted=%d pending=%d total=%lu\n",
    &machine->ring, machine->metrics.ops_unsubmitted, machine->metrics.ops_pending, machine->metrics.total_ops
  );

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
    DEBUG_PRINTF("-> %p io_uring_enter\n", &machine->ring);
    int ret = io_uring_enter(machine->ring.ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
    DEBUG_PRINTF("<- %p io_uring_enter: result=%d\n", &machine->ring, ret);
    if (ret < 0)
      rb_syserr_fail(-ret, strerror(-ret));

    overflow_checked = true;
    goto iterate;
  }

done:
  DEBUG_PRINTF("<- %p um_process_ready_cqes: total_processed=%u\n", &machine->ring, total_count);

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
  if (ctx->machine->metrics.ops_unsubmitted) {
    DEBUG_PRINTF("-> %p io_uring_submit_and_wait_timeout: unsubmitted=%d pending=%d total=%lu\n",
      &ctx->machine->ring, ctx->machine->metrics.ops_unsubmitted, ctx->machine->metrics.ops_pending,
      ctx->machine->metrics.total_ops
    );

    // Attn: The io_uring_submit_and_wait_timeout will not return -EINTR if
    // interrupted with a signal. We can detect this by testing ctx->cqe for
    // NULL.
    //
    // https://github.com/axboe/liburing/issues/1280
    int ret = io_uring_submit_and_wait_timeout(&ctx->machine->ring, &ctx->cqe, ctx->wait_nr, NULL, NULL);
    ctx->machine->metrics.ops_unsubmitted = 0;
    DEBUG_PRINTF("<- %p io_uring_submit_and_wait_timeout: result=%d\n", &ctx->machine->ring, ret);
    ctx->result = (ret > 0 && !ctx->cqe) ? -EINTR : ret;
  }
  else {
    DEBUG_PRINTF("-> %p io_uring_wait_cqes: unsubmitted=%d pending=%d total=%lu\n",
      &ctx->machine->ring, ctx->machine->metrics.ops_unsubmitted, ctx->machine->metrics.ops_pending,
      ctx->machine->metrics.total_ops
    );
    ctx->result = io_uring_wait_cqes(&ctx->machine->ring, &ctx->cqe, ctx->wait_nr, NULL, NULL);
    DEBUG_PRINTF("<- %p io_uring_wait_cqes: result=%d\n", &ctx->machine->ring, ctx->result);
  }
  return NULL;
}

inline void um_profile_wait_cqe_pre(struct um *machine, double *time_monotonic0, VALUE *fiber) {
  // *fiber = rb_fiber_current();
  *time_monotonic0 = um_get_time_monotonic();
  // double time_cpu = um_get_time_cpu();
  // double elapsed = time_cpu - machine->metrics.time_last_cpu;
  // um_update_fiber_time_run(fiber, time_monotonic0, elapsed);
  // machine->metrics.time_last_cpu = time_cpu;
}

inline void um_profile_wait_cqe_post(struct um *machine, double time_monotonic0, VALUE fiber) {
  // double time_cpu = um_get_time_cpu();
  double elapsed = um_get_time_monotonic() - time_monotonic0;
  // um_update_fiber_last_time(fiber, cpu_time1);
  machine->metrics.time_total_wait += elapsed;
  // machine->metrics.time_last_cpu = time_cpu;
}

inline void *um_wait_for_sidecar_signal(void *ptr) {
  struct um *machine = ptr;
  um_sidecar_signal_wait(machine);
  return NULL;
}

// Waits for the given minimum number of completion entries. The wait_nr is
// either 1 - where we wait for at least one CQE to be ready, or 0, where we
// don't wait, and just process any CQEs that already ready.
static inline void um_wait_for_and_process_ready_cqes(struct um *machine, int wait_nr) {
  struct wait_for_cqe_ctx ctx = { .machine = machine, .cqe = NULL, .wait_nr = wait_nr };
  machine->metrics.total_waits++;

  if (machine->sidecar_mode) {
    // fprintf(stderr, ">> sidecar wait cqes (unsubmitted: %d)\n", machine->metrics.ops_unsubmitted);
    if (machine->metrics.ops_unsubmitted) {
      io_uring_submit(&machine->ring);
      machine->metrics.ops_unsubmitted = 0;
    }
    if (wait_nr) {
      // fprintf(stderr, ">> um_wait_for_sidecar_signal\n");
      rb_thread_call_without_gvl(um_wait_for_sidecar_signal, (void *)machine, RUBY_UBF_PROCESS, 0);
      // fprintf(stderr, "<< um_wait_for_sidecar_signal\n");
    }
    um_process_ready_cqes(machine);
    // fprintf(stderr, "<< sidecar wait cqes\n");
  } 
  else {
    double time_monotonic0 = 0.0;
    VALUE fiber;
    if (machine->profile_mode) um_profile_wait_cqe_pre(machine, &time_monotonic0, &fiber);
    rb_thread_call_without_gvl(um_wait_for_cqe_without_gvl, (void *)&ctx, RUBY_UBF_IO, 0);
    if (machine->profile_mode) um_profile_wait_cqe_post(machine, time_monotonic0, fiber);

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
}

inline void um_profile_switch(struct um *machine, VALUE next_fiber) {
  // *current_fiber = rb_fiber_current();
  // double time_cpu = um_get_time_cpu();
  // double elapsed = time_cpu - machine->metrics.time_last_cpu;
  // um_update_fiber_time_run(cur_fiber, time_cpu, elapsed);
  // um_update_fiber_time_wait(next_fiber, time_cpu);
  // machine->metrics.time_last_cpu = time_cpu;
}

inline VALUE process_runqueue_op(struct um *machine, struct um_op *op) {
  DEBUG_PRINTF("-> %p process_runqueue_op: op %p\n", &machine->ring, op);

  machine->metrics.total_switches++;
  VALUE fiber = op->fiber;
  VALUE value = op->value;

  if (unlikely(op->flags & OP_F_TRANSIENT))
    um_op_free(machine, op);

  if (machine->profile_mode) um_profile_switch(machine, fiber);
  VALUE ret = rb_fiber_transfer(fiber, 1, &value);
  RB_GC_GUARD(value);
  RB_GC_GUARD(ret);
  return ret;
}

inline VALUE um_switch(struct um *machine) {
  DEBUG_PRINTF("-> %p um_switch: unsubmitted=%d pending=%d total=%lu\n",
    &machine->ring, machine->metrics.ops_unsubmitted, machine->metrics.ops_pending,
    machine->metrics.total_ops
  );

  while (true) {
    struct um_op *op = um_runqueue_shift(machine);
    if (op) {
      if (unlikely(op->flags & OP_F_RUNQUEUE_SKIP)) continue;

      // in test mode we want to process I/O on each snooze
      if (unlikely(machine->test_mode && (op->kind == OP_SCHEDULE))) {
        um_wait_for_and_process_ready_cqes(machine, 0);
      }
      return process_runqueue_op(machine, op);
    }

    um_wait_for_and_process_ready_cqes(machine, 1);
  }
}

inline VALUE um_yield(struct um *machine) {
  VALUE fiber = rb_fiber_current();
  rb_set_add(machine->pending_fibers, fiber);
  VALUE ret = um_switch(machine);
  rb_set_delete(machine->pending_fibers, fiber);
  return ret;
}

void um_cancel_op(struct um *machine, struct um_op *op) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_cancel64(sqe, (long long)op, 0);
}

inline void um_cancel_and_wait(struct um *machine, struct um_op *op) {
  um_cancel_op(machine, op);

  VALUE fiber = rb_fiber_current();
  rb_set_add(machine->pending_fibers, fiber);
  while (!um_op_completed_p(op)) {
    um_switch(machine);
  }
  rb_set_delete(machine->pending_fibers, fiber);
}

inline int um_check_completion(struct um *machine, struct um_op *op) {
  if (!um_op_completed_p(op)) {
    um_cancel_and_wait(machine, op);
    return 0;
  }

  um_raise_on_error_result(op->result.res);
  return 1;
}

VALUE um_wakeup(struct um *machine) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_nop(sqe);
  io_uring_submit(&machine->ring);
  return Qnil;
}

inline void um_prep_op(struct um *machine, struct um_op *op, enum um_op_kind kind, unsigned flags) {
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

  struct um_queue *queue;
  void *read_buf;
  int read_maxlen;
  struct __kernel_timespec ts;
  int flags;
};

VALUE um_timeout_complete(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;

  if (!um_op_completed_p(ctx->op)) {
    um_cancel_op(ctx->machine, ctx->op);
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

  VALUE ret = um_yield(machine);

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

VALUE um_read(struct um *machine, int fd, VALUE buffer, size_t maxlen, ssize_t buffer_offset, __u64 file_offset) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, buffer_offset);
  io_uring_prep_read(sqe, fd, ptr, maxlen, file_offset);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op)) {
    um_update_read_buffer(machine, buffer, buffer_offset, op.result.res, op.result.flags);
    ret = INT2NUM(op.result.res);

  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

size_t um_read_raw(struct um *machine, int fd, char *buffer, size_t maxlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_READ, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_read(sqe, fd, buffer, maxlen, -1);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op)) {
    return op.result.res;
  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return 0;
}

VALUE um_write(struct um *machine, int fd, VALUE buffer, size_t len, __u64 file_offset) {
  const void *base;
  size_t size;
  um_get_buffer_bytes_for_writing(buffer, &base, &size);
  if ((len == (size_t)-1) || (len > size)) len = size;
  if (unlikely(!len)) return INT2NUM(0);

  struct um_op op;
  um_prep_op(machine, &op, OP_WRITE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  io_uring_prep_write(sqe, fd, base, len, file_offset);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_writev(struct um *machine, int fd, int argc, VALUE *argv) {
  __u64 file_offset = -1;
  if (TYPE(argv[argc - 1]) == T_FIXNUM) {
    file_offset = NUM2UINT(argv[argc - 1]);
    argc--;
  }

  size_t total_len, len;
  struct iovec *iovecs = um_alloc_iovecs_for_writing(argc, argv, &total_len);
  struct iovec *iovecs_ptr = iovecs;
  int iovecs_len = argc;
  struct um_op op;
  VALUE ret = Qnil;
  int writev_res = 0;

  if (unlikely(!total_len)) {
    free(iovecs);
    return INT2NUM(0);
  }
  len = total_len;
  while (len) {
    um_prep_op(machine, &op, OP_WRITEV, 0);
    struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
    io_uring_prep_writev(sqe, fd, iovecs_ptr, iovecs_len, file_offset);

    ret = um_yield(machine);

    int completed = um_op_completed_p(&op);
    if (unlikely(!completed)) goto cancelled;
  
    writev_res = op.result.res;
    if (unlikely(writev_res < 0)) goto done;
      
    len -= writev_res;
    if (len) {
      um_advance_iovecs_for_writing(&iovecs_ptr, &iovecs_len, (size_t)writev_res);
      if (file_offset != (__u64)-1) file_offset += writev_res;
    }
  }

cancelled:
  um_cancel_and_wait(machine, &op);
done:
  free(iovecs);
  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  um_raise_on_error_result(writev_res);
  return INT2NUM(total_len);
}

VALUE um_write_async(struct um *machine, int fd, VALUE buffer, size_t len, __u64 file_offset) {
  const void *base;
  size_t size;
  um_get_buffer_bytes_for_writing(buffer, &base, &size);
  if ((len == (size_t)-1) || (len > size)) len = size;
  if (unlikely(!len)) return INT2NUM(0);

  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_WRITE_ASYNC, OP_F_TRANSIENT | OP_F_FREE_ON_COMPLETE);
  RB_OBJ_WRITE(machine->self, &op->value, buffer);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_write(sqe, fd, base, len, file_offset);
  um_op_transient_add(machine, op);

  return buffer;
}

VALUE um_close(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_CLOSE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_close(sqe, fd);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(fd);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_close_async(struct um *machine, int fd) {
  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_CLOSE_ASYNC, OP_F_FREE_ON_COMPLETE);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_close(sqe, fd);

  return INT2NUM(fd);
}

VALUE um_accept(struct um *machine, int fd) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_accept(sqe, fd, NULL, NULL, 0);

  VALUE ret = um_yield(machine);

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

  VALUE ret = um_yield(machine);

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

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_send(struct um *machine, int fd, VALUE buffer, size_t len, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_SEND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  const void *base;
  size_t size;
  um_get_buffer_bytes_for_writing(buffer, &base, &size);
  if ((len == (size_t)-1) || (len > size)) len = size;

  io_uring_prep_send(sqe, fd, base, len, flags);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
  // int ret = write(fd, base, len);
  // return UINT2NUM(ret);
}

// for some reason we don't get this define from liburing/io_uring.h
#define IORING_SEND_VECTORIZED		(1U << 5)

VALUE um_sendv(struct um *machine, int fd, int argc, VALUE *argv) {
  struct iovec *iovecs = um_alloc_iovecs_for_writing(argc, argv, NULL);
  struct um_op op;
  um_prep_op(machine, &op, OP_SEND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  io_uring_prep_send(sqe, fd, iovecs, argc, MSG_NOSIGNAL | MSG_WAITALL);
  sqe->ioprio |= IORING_SEND_VECTORIZED;

  VALUE ret = um_yield(machine);

  int completed = um_op_completed_p(&op);
  if (unlikely(!completed)) um_cancel_and_wait(machine, &op);
  free(iovecs);
  if (likely(completed)) {
    um_raise_on_error_result(op.result.res);
    ret = INT2NUM(op.result.res);
  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_send_bundle(struct um *machine, int fd, int bgid, VALUE strings) {
  um_add_strings_to_buffer_ring(machine, bgid, strings);

  struct um_op op;
  um_prep_op(machine, &op, OP_SEND_BUNDLE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

	io_uring_prep_send_bundle(sqe, fd, 0, MSG_NOSIGNAL | MSG_WAITALL);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = bgid;

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_recv(struct um *machine, int fd, VALUE buffer, size_t maxlen, int flags) {
  struct um_op op;
  um_prep_op(machine, &op, OP_RECV, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  void *ptr = um_prepare_read_buffer(buffer, maxlen, 0);

  io_uring_prep_recv(sqe, fd, ptr, maxlen, flags);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op)) {
    um_update_read_buffer(machine, buffer, 0, op.result.res, op.result.flags);
    ret = INT2NUM(op.result.res);
  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_bind(struct um *machine, int fd, struct sockaddr *addr, socklen_t addrlen) {
  struct um_op op;
  um_prep_op(machine, &op, OP_BIND, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_bind(sqe, fd, addr, addrlen);

  VALUE ret = um_yield(machine);

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

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_getsockopt(struct um *machine, int fd, int level, int opt) {
  VALUE ret = Qnil;
  int value;

  struct um_op op;
  um_prep_op(machine, &op, OP_GETSOCKOPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, level, opt, &value, sizeof(value));

  ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(value);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_setsockopt(struct um *machine, int fd, int level, int opt, int value) {
  VALUE ret = Qnil;

  struct um_op op;
  um_prep_op(machine, &op, OP_SETSOCKOPT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, level, opt, &value, sizeof(value));

  ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

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

  ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

VALUE um_shutdown_async(struct um *machine, int fd, int how) {
  struct um_op *op = um_op_alloc(machine);
  um_prep_op(machine, op, OP_SHUTDOWN_ASYNC, OP_F_FREE_ON_COMPLETE);

  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  io_uring_prep_shutdown(sqe, fd, how);

  return INT2NUM(fd);
}

VALUE um_open(struct um *machine, VALUE pathname, int flags, int mode) {
  struct um_op op;
  um_prep_op(machine, &op, OP_OPEN, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_open(sqe, StringValueCStr(pathname), flags, mode);

  VALUE ret = um_yield(machine);

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

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  RB_GC_GUARD(op.fiber);
  RB_GC_GUARD(op.value);
  return ret;
}

static inline void prepare_select_poll_ops(struct um *machine, uint *idx, struct um_op *ops, VALUE fds, uint len, uint flags, uint event) {
  for (uint i = 0; i < len; i++) {
    struct um_op *op = ops + ((*idx)++);
    um_prep_op(machine, op, OP_POLL, flags | OP_F_IGNORE_CANCELED);
    struct io_uring_sqe *sqe = um_get_sqe(machine, op);
    VALUE fd = rb_ary_entry(fds, i);
    RB_OBJ_WRITE(machine->self, &op->value, fd);
    io_uring_prep_poll_add(sqe, NUM2INT(fd), event);
  }
}

VALUE um_select_single(struct um *machine, VALUE rfds, VALUE wfds, VALUE efds, uint rfds_len, uint wfds_len, uint efds_len) {
  struct um_op op;
  uint idx = 0;
  if (rfds_len)
    prepare_select_poll_ops(machine, &idx, &op, rfds, rfds_len, OP_F_SELECT_POLLIN, POLLIN);
  else if (wfds_len)
    prepare_select_poll_ops(machine, &idx, &op, wfds, wfds_len, OP_F_SELECT_POLLOUT, POLLOUT);
  else if (efds_len)
    prepare_select_poll_ops(machine, &idx, &op, efds, efds_len, OP_F_SELECT_POLLPRI, POLLPRI);
  assert(idx == 1);

  VALUE ret = um_yield(machine);

  um_check_completion(machine, &op);
  RAISE_IF_EXCEPTION(ret);

  if (op.flags & OP_F_SELECT_POLLIN)
    return rb_ary_new3(3, rb_ary_new3(1, ret), rb_ary_new(), rb_ary_new());
  else if (op.flags & OP_F_SELECT_POLLOUT)
    return rb_ary_new3(3, rb_ary_new(), rb_ary_new3(1, ret), rb_ary_new());
  else
    return rb_ary_new3(3, rb_ary_new(), rb_ary_new(), rb_ary_new3(1, ret));

  RB_GC_GUARD(ret);
}

VALUE um_select(struct um *machine, VALUE rfds, VALUE wfds, VALUE efds) {
  uint rfds_len = RARRAY_LEN(rfds);
  uint wfds_len = RARRAY_LEN(wfds);
  uint efds_len = RARRAY_LEN(efds);
  uint total_len = rfds_len + wfds_len + efds_len;
  if (total_len == 1)
    return um_select_single(machine, rfds, wfds, efds, rfds_len, wfds_len, efds_len);

  if (unlikely(!total_len))
    return rb_ary_new3(3, rb_ary_new(), rb_ary_new(), rb_ary_new());

  struct um_op *ops = malloc(sizeof(struct um_op) * total_len);
  uint idx = 0;
  prepare_select_poll_ops(machine, &idx, ops, rfds, rfds_len, OP_F_SELECT_POLLIN, POLLIN);
  prepare_select_poll_ops(machine, &idx, ops, wfds, wfds_len, OP_F_SELECT_POLLOUT, POLLOUT);
  prepare_select_poll_ops(machine, &idx, ops, efds, efds_len, OP_F_SELECT_POLLPRI, POLLPRI);
  assert(idx == total_len);

  VALUE ret = um_yield(machine);
  if (unlikely(um_value_is_exception_p(ret))) {
    free(ops);
    um_raise_exception(ret);
  }

  VALUE rfds_out = rb_ary_new();
  VALUE wfds_out = rb_ary_new();
  VALUE efds_out = rb_ary_new();

  int error_code = 0;
  uint pending = total_len;
  for (uint i = 0; i < total_len; i++) {
    if (um_op_completed_p(&ops[i])) {
      ops[i].flags |= OP_F_RUNQUEUE_SKIP;
      pending--;

      if (unlikely((ops[i].result.res < 0) && !error_code)) {
        error_code = ops[i].result.res;
      }
      else {
        if (ops[i].flags & OP_F_SELECT_POLLIN)  rb_ary_push(rfds_out, ops[i].value);
        if (ops[i].flags & OP_F_SELECT_POLLOUT) rb_ary_push(wfds_out, ops[i].value);
        if (ops[i].flags & OP_F_SELECT_POLLPRI) rb_ary_push(efds_out, ops[i].value);
      }
    }
    else {
      ops[i].flags |= OP_F_CANCELED;
      um_cancel_op(machine, &ops[i]);
    }
  }

  while (pending) {
    um_wait_for_and_process_ready_cqes(machine, 1);

    for (uint i = 0; i < total_len; i++) {
      struct um_op *op = ops + i;
      if (op->flags & OP_F_CANCELED && um_op_completed_p(op)) {
        pending--;
      }
    }
  }
  free(ops);

  if (error_code)
    um_raise_on_error_result(error_code);

  return rb_ary_new3(3, rfds_out, wfds_out, efds_out);

  RB_GC_GUARD(rfds_out);
  RB_GC_GUARD(wfds_out);
  RB_GC_GUARD(efds_out);
}

VALUE um_waitid(struct um *machine, int idtype, int id, int options) {
  struct um_op op;
  um_prep_op(machine, &op, OP_WAITID, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  siginfo_t infop;
  io_uring_prep_waitid(sqe, idtype, id, &infop, options, 0);

  VALUE ret = um_yield(machine);

  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);

  return rb_ary_new_from_args(
    3, INT2NUM(infop.si_pid), INT2NUM(infop.si_status), INT2NUM(infop.si_code)
  );
}

#ifdef HAVE_RB_PROCESS_STATUS_NEW
VALUE um_waitid_status(struct um *machine, int idtype, int id, int options) {
  struct um_op op;
  um_prep_op(machine, &op, OP_WAITID, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);

  siginfo_t infop;
  io_uring_prep_waitid(sqe, idtype, id, &infop, options | WNOWAIT, 0);

  VALUE ret = um_yield(machine);
  if (um_check_completion(machine, &op))
    ret = INT2NUM(op.result.res);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);

  return rb_process_status_new(infop.si_pid, (infop.si_status & 0xff) << 8, 0);
}
#endif

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

  VALUE ret = um_yield(machine);

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
    VALUE ret = um_yield(ctx->machine);
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
        rb_syserr_fail(-result->res, strerror(-result->res));
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

VALUE accept_into_queue_start(VALUE arg) {
  struct op_ctx *ctx = (struct op_ctx *)arg;
  struct io_uring_sqe *sqe = um_get_sqe(ctx->machine, ctx->op);
  io_uring_prep_multishot_accept(sqe, ctx->fd, NULL, NULL, 0);

  while (true) {
    VALUE ret = um_yield(ctx->machine);
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
        rb_syserr_fail(-result->res, strerror(-result->res));
      }
      um_queue_push(ctx->machine, ctx->queue, INT2NUM(result->res));
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

VALUE um_accept_into_queue(struct um *machine, int fd, VALUE queue) {
  struct um_op op;
  um_prep_op(machine, &op, OP_ACCEPT_MULTISHOT, OP_F_MULTISHOT);

  struct op_ctx ctx = {
    .machine = machine, .op = &op, .fd = fd, .queue = Queue_data(queue), .read_buf = NULL
  };
  return rb_ensure(accept_into_queue_start, (VALUE)&ctx, multishot_complete, (VALUE)&ctx);
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

    VALUE ret = um_yield(ctx->machine);
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
    VALUE ret = um_yield(ctx->machine);
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
    VALUE ret = um_switch(ctx->machine);
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

extern VALUE SYM_size;
extern VALUE SYM_total_ops;
extern VALUE SYM_total_switches;
extern VALUE SYM_total_waits;
extern VALUE SYM_ops_pending;
extern VALUE SYM_ops_unsubmitted;
extern VALUE SYM_ops_runqueue;
extern VALUE SYM_ops_free;
extern VALUE SYM_ops_transient;
extern VALUE SYM_time_total_cpu;
extern VALUE SYM_time_total_wait;

VALUE um_metrics(struct um *machine, struct um_metrics *metrics) {
  VALUE hash = rb_hash_new();

  rb_hash_aset(hash, SYM_size,            UINT2NUM(machine->size));

  rb_hash_aset(hash, SYM_total_ops,       ULONG2NUM(metrics->total_ops));
  rb_hash_aset(hash, SYM_total_switches,  ULONG2NUM(metrics->total_switches));
  rb_hash_aset(hash, SYM_total_waits,     ULONG2NUM(metrics->total_waits));

  rb_hash_aset(hash, SYM_ops_pending,     UINT2NUM(metrics->ops_pending));
  rb_hash_aset(hash, SYM_ops_unsubmitted, UINT2NUM(metrics->ops_unsubmitted));
  rb_hash_aset(hash, SYM_ops_runqueue,    UINT2NUM(metrics->ops_runqueue));
  rb_hash_aset(hash, SYM_ops_free,        UINT2NUM(metrics->ops_free));
  rb_hash_aset(hash, SYM_ops_transient,   UINT2NUM(metrics->ops_transient));

  if (machine->profile_mode) {
    double total_cpu = um_get_time_cpu() - metrics->time_first_cpu;
    rb_hash_aset(hash, SYM_time_total_cpu,  DBL2NUM(total_cpu));
    rb_hash_aset(hash, SYM_time_total_wait, DBL2NUM(metrics->time_total_wait));
  }

  return hash;
  RB_GC_GUARD(hash);
}
