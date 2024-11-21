#include "um.h"

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
}

inline void um_op_transient_remove(struct um *machine, struct um_op *op) {
  if (op->prev)
    op->prev->next = op->next;
  if (op->next)
    op->next->prev = op->prev;

  if (machine->transient_head == op)
    machine->transient_head = op->next;
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
}

inline struct um_op *um_runqueue_shift(struct um *machine) {
  struct um_op *op = machine->runqueue_head;
  if (!op) return NULL;

  machine->runqueue_head = op->next;
  if (!machine->runqueue_head)
    machine->runqueue_tail = NULL;
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
  return malloc(sizeof(struct um_op_result));
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
    return op;
  }
  return malloc(sizeof(struct um_op));
}

inline void um_op_free(struct um *machine, struct um_op *op) {
  op->next = machine->op_freelist;
  machine->op_freelist = op;
}
