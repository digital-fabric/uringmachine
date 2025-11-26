#include "um.h"
#include <stdatomic.h>
#include <linux/futex.h>

#define FUTEX2_SIZE_U32		0x02

void um_futex_wait(struct um *machine, uint32_t *futex, uint32_t expect) {
  struct um_op op;
  um_prep_op(machine, &op, OP_FUTEX_WAIT, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  io_uring_prep_futex_wait(
    sqe, (uint32_t *)futex, expect, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );

  VALUE ret = um_fiber_switch(machine);
  if (!um_op_completed_p(&op))
    um_cancel_and_wait(machine, &op);
  else {
    if (op.result.res != -EAGAIN)
      um_raise_on_error_result(op.result.res);
  }

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
}

void um_futex_wake(struct um *machine, uint32_t *futex, uint32_t num_waiters) {
  struct um_op op;
  um_prep_op(machine, &op, OP_FUTEX_WAKE, 0);
  struct io_uring_sqe *sqe = um_get_sqe(machine, &op);
  // submit futex_wait
  io_uring_prep_futex_wake(
    sqe, (uint32_t *)futex, num_waiters, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );

  VALUE ret = um_fiber_switch(machine);
  um_check_completion(machine, &op);

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
}

void um_futex_wake_transient(struct um *machine, uint32_t *futex, uint32_t num_waiters) {
  struct io_uring_sqe *sqe = um_get_sqe(machine, NULL);
  io_uring_prep_futex_wake(
    sqe, (uint32_t *)futex, num_waiters, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );
}


#define MUTEX_LOCKED    1
#define MUTEX_UNLOCKED  0

void um_mutex_init(struct um_mutex *mutex) {
  mutex->state = MUTEX_UNLOCKED;
  mutex->num_waiters = 0;
}

inline void um_mutex_lock(struct um *machine, struct um_mutex *mutex) {
  mutex->num_waiters++;
  while (mutex->state == MUTEX_LOCKED) {
    um_futex_wait(machine, &mutex->state, MUTEX_LOCKED);
  }
  mutex->num_waiters--;
  mutex->state = MUTEX_LOCKED;
}

inline void um_mutex_unlock(struct um *machine, struct um_mutex *mutex) {
  mutex->state = MUTEX_UNLOCKED;

  if (mutex->num_waiters)
    // Wake up 1 waiting fiber
    um_futex_wake(machine, &mutex->state, 1);
}

struct sync_ctx {
  struct um *machine;
  struct um_mutex *mutex;
};

VALUE synchronize_start(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_lock(ctx->machine, ctx->mutex);
  return rb_yield(Qnil);
}

VALUE synchronize_complete(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_unlock(ctx->machine, ctx->mutex);
  return Qnil;
}

inline VALUE um_mutex_synchronize(struct um *machine, struct um_mutex *mutex) {
  struct sync_ctx ctx = { .machine = machine, .mutex = mutex };
  return rb_ensure(synchronize_start, (VALUE)&ctx, synchronize_complete, (VALUE)&ctx);
}

#define QUEUE_EMPTY 0
#define QUEUE_READY 1

inline void um_queue_init(struct um_queue *queue) {
  queue->head = queue->tail = queue->free_head = NULL;
  queue->state = QUEUE_EMPTY;
  queue->count = 0;
}

inline void um_queue_free(struct um_queue *queue) {
  struct um_queue_entry *entry = queue->head;
  while (entry) {
    struct um_queue_entry *next = entry->next;
    free(entry);
    entry = next;
  }

  entry = queue->free_head;
  while (entry) {
    struct um_queue_entry *next = entry->next;
    free(entry);
    entry = next;
  }
}

inline void um_queue_mark(struct um_queue *queue) {
  rb_gc_mark_movable(queue->self);
  struct um_queue_entry *entry = queue->head;
  while (entry) {
    rb_gc_mark_movable(entry->value);
    entry = entry->next;
  }
}

inline void um_queue_compact(struct um_queue *queue) {
  queue->self = rb_gc_location(queue->self);
  struct um_queue_entry *entry = queue->head;
  while (entry) {
    entry->value = rb_gc_location(entry->value);
    entry = entry->next;
  }
}

inline struct um_queue_entry *um_queue_entry_checkout(struct um_queue *queue) {
  struct um_queue_entry *entry = queue->free_head;
  if (entry) {
    queue->free_head = entry->next;
  }
  else
    entry = malloc(sizeof(struct um_queue_entry));
  return entry;
}

inline void um_queue_entry_checkin(struct um_queue *queue, struct um_queue_entry *entry) {
  entry->next = queue->free_head;
  queue->free_head = entry;
}

static inline void queue_add_head(struct um_queue *queue, VALUE value) {
  struct um_queue_entry *entry = um_queue_entry_checkout(queue);

  entry->next = queue->head;
  if (queue->head) {
    queue->head->prev = entry;
    queue->head = entry;
  }
  else
    queue->head = queue->tail = entry;
  entry->prev = NULL;
  RB_OBJ_WRITE(queue->self, &entry->value, value);
}

static inline void queue_add_tail(struct um_queue *queue, VALUE value) {
  struct um_queue_entry *entry = um_queue_entry_checkout(queue);

  entry->prev = queue->tail;
  if (queue->tail) {
    queue->tail->next = entry;
    queue->tail = entry;
  }
  else
    queue->head = queue->tail = entry;
  entry->next = NULL;
  RB_OBJ_WRITE(queue->self, &entry->value, value);
}

VALUE queue_remove_head(struct um_queue *queue) {
  struct um_queue_entry *entry = queue->head;
  queue->head = entry->next;
  if (!queue->head) queue->tail = NULL;

  VALUE v = entry->value;
  um_queue_entry_checkin(queue, entry);
  return v;

}

VALUE queue_remove_tail(struct um_queue *queue) {
  struct um_queue_entry *entry = queue->tail;
  queue->tail = entry->prev;
  if (!queue->tail) queue->head = NULL;

  VALUE v = entry->value;
  um_queue_entry_checkin(queue, entry);
  return v;
}

static inline VALUE um_queue_add(struct um *machine, struct um_queue *queue, VALUE value, int add_head) {
  if (add_head) queue_add_head(queue, value);
  else          queue_add_tail(queue, value);

  queue->count++;

  queue->state = QUEUE_READY;
  if (queue->num_waiters)
    um_futex_wake_transient(machine, &queue->state, 1);
  return queue->self;
}

VALUE um_queue_push(struct um *machine, struct um_queue *queue, VALUE value) {
  return um_queue_add(machine, queue, value, false);
}

VALUE um_queue_unshift(struct um *machine, struct um_queue *queue, VALUE value) {
  return um_queue_add(machine, queue, value, true);
}

enum queue_op { QUEUE_POP, QUEUE_SHIFT };

struct queue_wait_ctx {
  struct um *machine;
  struct um_queue *queue;
  enum queue_op op;
};

VALUE um_queue_remove_start(VALUE arg) {
  struct queue_wait_ctx *ctx = (struct queue_wait_ctx *)arg;

  ctx->queue->num_waiters++;
  while (ctx->queue->state == QUEUE_EMPTY) {
    um_futex_wait(ctx->machine, &ctx->queue->state, QUEUE_EMPTY);
  }

  if (ctx->queue->state != QUEUE_READY)
    rb_raise(rb_eRuntimeError, "Internal error: queue should be in ready state!");
  if (!ctx->queue->tail)
    rb_raise(rb_eRuntimeError, "Internal error: queue should be in ready state!");

  ctx->queue->count--;
  return (ctx->op == QUEUE_POP ? queue_remove_tail : queue_remove_head)(ctx->queue);
}

VALUE um_queue_remove_complete(VALUE arg) {
  struct queue_wait_ctx *ctx = (struct queue_wait_ctx *)arg;
  ctx->queue->num_waiters--;

  if (ctx->queue->num_waiters && ctx->queue->tail) {
    um_futex_wake_transient(ctx->machine, &ctx->queue->state, 1);
  }
  else if (!ctx->queue->tail) {
    ctx->queue->state = QUEUE_EMPTY;
  }

  return Qnil;
}

VALUE um_queue_pop(struct um *machine, struct um_queue *queue) {
  struct queue_wait_ctx ctx = { .machine = machine, .queue = queue, .op = QUEUE_POP };
  return rb_ensure(um_queue_remove_start, (VALUE)&ctx, um_queue_remove_complete, (VALUE)&ctx);
}

VALUE um_queue_shift(struct um *machine, struct um_queue *queue) {
  struct queue_wait_ctx ctx = { .machine = machine, .queue = queue, .op = QUEUE_SHIFT };
  return rb_ensure(um_queue_remove_start, (VALUE)&ctx, um_queue_remove_complete, (VALUE)&ctx);
}
