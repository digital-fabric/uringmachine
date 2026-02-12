#include "um.h"

#define UM_OP_ALLOC_BATCH_SIZE        256
#define UM_OP_RESULT_ALLOC_BATCH_SIZE 256

const char * um_op_kind_name(enum um_op_kind kind) {
  switch (kind) {
    case OP_TIMEOUT:            return "OP_TIMEOUT";
    case OP_SCHEDULE:           return "OP_SCHEDULE";
    case OP_SLEEP:              return "OP_SLEEP";
    case OP_OPEN:               return "OP_OPEN";
    case OP_READ:               return "OP_READ";
    case OP_WRITE:              return "OP_WRITE";
    case OP_WRITEV:             return "OP_WRITEV";
    case OP_WRITE_ASYNC:        return "OP_WRITE_ASYNC";
    case OP_CLOSE:              return "OP_CLOSE";
    case OP_CLOSE_ASYNC:        return "OP_CLOSE_ASYNC";
    case OP_STATX:              return "OP_STATX";
    case OP_ACCEPT:             return "OP_ACCEPT";
    case OP_RECV:               return "OP_RECV";
    case OP_SEND:               return "OP_SEND";
    case OP_SENDV:              return "OP_SENDV";
    case OP_SEND_BUNDLE:        return "OP_SEND_BUNDLE";
    case OP_SOCKET:             return "OP_SOCKET";
    case OP_CONNECT:            return "OP_CONNECT";
    case OP_BIND:               return "OP_BIND";
    case OP_LISTEN:             return "OP_LISTEN";
    case OP_GETSOCKOPT:         return "OP_GETSOCKOPT";
    case OP_SETSOCKOPT:         return "OP_SETSOCKOPT";
    case OP_SHUTDOWN:           return "OP_SHUTDOWN";
    case OP_SHUTDOWN_ASYNC:     return "OP_SHUTDOWN_ASYNC";
    case OP_POLL:               return "OP_POLL";
    case OP_WAITID:             return "OP_WAITID";
    case OP_FUTEX_WAIT:         return "OP_FUTEX_WAIT";
    case OP_FUTEX_WAKE:         return "OP_FUTEX_WAKE";
    case OP_ACCEPT_MULTISHOT:   return "OP_ACCEPT_MULTISHOT";
    case OP_READ_MULTISHOT:     return "OP_READ_MULTISHOT";
    case OP_RECV_MULTISHOT:     return "OP_RECV_MULTISHOT";
    case OP_TIMEOUT_MULTISHOT:  return "OP_TIMEOUT_MULTISHOT";
    default:                    return "UNKNOWN_OP_KIND";
  }
}

inline void um_op_clear(struct um *machine, struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  op->fiber = Qnil;
  op->value = Qnil;
  op->async_op = Qnil;
}

inline void um_op_transient_add(struct um *machine, struct um_op *op) {
  if (machine->transient_head) {
    op->next = machine->transient_head;
    machine->transient_head->prev = op;
  }
  machine->transient_head = op;
  machine->metrics.ops_transient++;
}

inline void um_op_transient_remove(struct um *machine, struct um_op *op) {
  op->flags &= ~OP_F_TRANSIENT;
  if (op->prev)
    op->prev->next = op->next;
  if (op->next)
    op->next->prev = op->prev;

  if (machine->transient_head == op)
    machine->transient_head = op->next;
  machine->metrics.ops_transient--;
}

inline void um_runqueue_push(struct um *machine, struct um_op *op) {
  if (machine->runqueue_tail) {
    op->prev = machine->runqueue_tail;
    machine->runqueue_tail->next = op;
    machine->runqueue_tail = op;
  }
  else
    machine->runqueue_head = machine->runqueue_tail = op;
  op->next = NULL;
  machine->metrics.ops_runqueue++;
}

inline struct um_op *um_runqueue_shift(struct um *machine) {
  struct um_op *op = machine->runqueue_head;
  if (!op) return NULL;

  machine->runqueue_head = op->next;
  if (!machine->runqueue_head)
    machine->runqueue_tail = NULL;
  machine->metrics.ops_runqueue--;
  return op;
}

inline void um_op_list_mark(struct um *machine, struct um_op *head) {
  while (head) {
    struct um_op *next = head->next;
    rb_gc_mark_movable(head->fiber);
    rb_gc_mark_movable(head->value);
    rb_gc_mark_movable(head->async_op);
    head = next;
  }
}

inline void um_op_list_compact(struct um *machine, struct um_op *head) {
  while (head) {
    struct um_op *next = head->next;
    head->fiber = rb_gc_location(head->fiber);
    head->value = rb_gc_location(head->value);
    head->async_op = rb_gc_location(head->async_op);
    head = next;
  }
}

inline struct um_op_result *multishot_result_alloc(struct um *machine) {
  if (machine->result_freelist) {
    struct um_op_result *result = machine->result_freelist;
    machine->result_freelist = result->next;
    return result;
  }

  struct um_op_result *batch = malloc(sizeof(struct um_op_result) * UM_OP_RESULT_ALLOC_BATCH_SIZE);
  for (int i = 1; i < (UM_OP_RESULT_ALLOC_BATCH_SIZE - 1); i++) {
    batch[i].next = &batch[i + 1];
  }
  machine->result_freelist = batch + 1;
  return batch;
}

inline void multishot_result_free(struct um *machine, struct um_op_result *result) {
  result->next = machine->result_freelist;
  machine->result_freelist = result;
}

inline void um_op_multishot_results_push(struct um *machine, struct um_op *op, __s32 res, __u32 flags) {
  if (!op->multishot_result_count) {
    op->result.res    = res;
    op->result.flags  = flags;
    op->result.next   = NULL;
    op->multishot_result_tail = &op->result;
  }
  else {
    struct um_op_result *result = multishot_result_alloc(machine);
    result->res   = res;
    result->flags = flags;
    result->next  = NULL;
    op->multishot_result_tail->next = result;
    op->multishot_result_tail = result;
  }
  op->multishot_result_count++;
}

inline void um_op_multishot_results_clear(struct um *machine, struct um_op *op) {
  if (op->multishot_result_count < 1) return;

  struct um_op_result *result = op->result.next;
  while (result) {
    struct um_op_result *next = result->next;
    multishot_result_free(machine, result);
    result = next;
  }
  op->multishot_result_tail = NULL;
  op->multishot_result_count = 0;
}

inline struct um_op *um_op_alloc(struct um *machine) {
  if (machine->op_freelist) {
    struct um_op *op = machine->op_freelist;
    machine->op_freelist = op->next;
    machine->metrics.ops_free--;
    return op;
  }

  struct um_op *batch = malloc(sizeof(struct um_op) * UM_OP_ALLOC_BATCH_SIZE);
  for (int i = 1; i < (UM_OP_ALLOC_BATCH_SIZE - 1); i++) {
    batch[i].next = &batch[i + 1];
  }
  machine->op_freelist = batch + 1;
  machine->metrics.ops_free += (UM_OP_ALLOC_BATCH_SIZE - 1);
  return batch;
}

inline void um_op_free(struct um *machine, struct um_op *op) {
  op->next = machine->op_freelist;
  machine->op_freelist = op;
  machine->metrics.ops_free++;
}

inline struct um_op *um_op_acquire(struct um *machine) {
  return um_op_alloc(machine);
}

inline void um_op_release(struct um *machine, struct um_op *op) {
  op->ref_count--;
  if (op->ref_count) return;

  if (op->flags & OP_F_FREE_IOVECS) free(op->iovecs);
  if (op->flags & OP_F_MULTISHOT)
    um_op_multishot_results_clear(machine, op);
  um_op_free(machine, op);
}
