#include "um.h"
#include <stdatomic.h>
#include <linux/futex.h>

struct sync_ctx {
  struct um *machine;
  uint32_t *futex;
};

#define FUTEX2_SIZE_U32		0x02

void um_futex_wait(struct um *machine, uint32_t *futex, uint32_t expect) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SYNCHRONIZE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  // submit futex_wait
  io_uring_prep_futex_wait(
    sqe, (uint32_t *)futex, expect, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );
  um_await_op(machine, op, &result, NULL);
  if (result != -EAGAIN)
    um_raise_on_error_result(result);
}

void um_futex_wake(struct um *machine, uint32_t *futex, uint32_t num_waiters) {
  struct um_op *op = um_op_idle_checkout(machine, OP_SYNCHRONIZE);
  struct io_uring_sqe *sqe = um_get_sqe(machine, op);
  __s32 result = 0;

  // submit futex_wait
  io_uring_prep_futex_wake(
    sqe, (uint32_t *)futex, num_waiters, FUTEX_BITSET_MATCH_ANY,
		FUTEX2_SIZE_U32, 0
  );
  um_await_op(machine, op, &result, NULL);
  um_raise_on_error_result(result);
}

#define MUTEX_LOCKED    1
#define MUTEX_UNLOCKED  0

void um_mutex_init(uint32_t *futex) {
  *futex = MUTEX_UNLOCKED;
}

void um_mutex_lock(struct um *machine, uint32_t *futex) {
  while (*futex == MUTEX_LOCKED) {
    um_futex_wait(machine, futex, MUTEX_LOCKED);
  }
  *futex = MUTEX_LOCKED;
}

void um_mutex_unlock(struct um *machine, uint32_t *futex) {
  *futex = MUTEX_UNLOCKED;
  // Wake up 1 waiting fiber
  um_futex_wake(machine, futex, 1);
}

VALUE synchronize_begin(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_lock(ctx->machine, ctx->futex);
  return rb_yield(Qnil);
}

VALUE synchronize_ensure(VALUE arg) {
  struct sync_ctx *ctx = (struct sync_ctx *)arg;
  um_mutex_unlock(ctx->machine, ctx->futex);
  return Qnil;
}

VALUE um_mutex_synchronize(struct um *machine, uint32_t *futex) {
  struct sync_ctx ctx = { .machine = machine, .futex = futex };
  return rb_ensure(synchronize_begin, (VALUE)&ctx, synchronize_ensure, (VALUE)&ctx);
}

#define QUEUE_EMPTY 0
#define QUEUE_READY 1

void um_queue_init(struct um_queue *queue) {
  queue->head = queue->tail = queue->free_head = NULL;
  queue->state = QUEUE_EMPTY;
}

void um_queue_free(struct um_queue *queue) {
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

  free(queue);
}

void um_queue_mark(struct um_queue *queue) {
  rb_gc_mark_movable(queue->self);
  struct um_queue_entry *entry = queue->head;
  while (entry) {
    rb_gc_mark_movable(entry->value);
    entry = entry->next;
  }
}

void um_queue_compact(struct um_queue *queue) {
  queue->self = rb_gc_location(queue->self);
  struct um_queue_entry *entry = queue->head;
  while (entry) {
    entry->value = rb_gc_location(entry->value);
    entry = entry->next;
  }
}

struct um_queue_entry *um_queue_entry_checkout(struct um_queue *queue) {
  struct um_queue_entry *entry = queue->free_head;
  if (entry) {
    queue->free_head = entry->next;
  }
  else
    entry = malloc(sizeof(struct um_queue_entry));
  return entry;
}

void um_queue_entry_checkin(struct um_queue *queue, struct um_queue_entry *entry) {
  entry->next = queue->free_head;
  queue->free_head = entry;
}

VALUE um_queue_push(struct um *machine, struct um_queue *queue, VALUE value) {
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

  queue->state = QUEUE_READY;
  if (queue->num_waiters)
    um_futex_wake(machine, &queue->state, 1);
  return queue->self;
}

struct queue_wait_ctx {
  struct um *machine;
  struct um_queue *queue;
};

VALUE um_queue_wait_begin(VALUE arg) {
  struct queue_wait_ctx *ctx = (struct queue_wait_ctx *)arg;
  
  ctx->queue->num_waiters++;
  while (ctx->queue->state == QUEUE_EMPTY) {
    um_futex_wait(ctx->machine, &ctx->queue->state, QUEUE_EMPTY);
  }

  if (ctx->queue->state != QUEUE_READY)
    rb_raise(rb_eRuntimeError, "Internal error: queue should be in ready state!");
  if (!ctx->queue->tail)
    rb_raise(rb_eRuntimeError, "Internal error: queue should be in ready state!");

  struct um_queue_entry *entry = ctx->queue->tail;
  ctx->queue->tail = entry->prev;
  if (!ctx->queue->tail) ctx->queue->head = NULL;

  VALUE v = entry->value;
  um_queue_entry_checkin(ctx->queue, entry);
  return v;
}

VALUE um_queue_wait_ensure(VALUE arg) {
  struct queue_wait_ctx *ctx = (struct queue_wait_ctx *)arg;

  ctx->queue->num_waiters--;

  if (ctx->queue->num_waiters && ctx->queue->tail) {
    um_futex_wake(ctx->machine, &ctx->queue->state, 1);
  }
  else if (!ctx->queue->tail) {
    ctx->queue->state = QUEUE_EMPTY;
  }

  return Qnil;
}

VALUE um_queue_pop(struct um *machine, struct um_queue *queue) {
  struct queue_wait_ctx ctx = {
    .machine = machine, 
    .queue = queue
  };
  return rb_ensure(um_queue_wait_begin, (VALUE)&ctx, um_queue_wait_ensure, (VALUE)&ctx);
}
