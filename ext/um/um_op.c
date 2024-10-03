#include "um.h"

inline void um_op_clear(struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  op->fiber = op->resume_value = Qnil;
}

inline struct um_op *um_op_checkout(struct um *machine) {
  machine->pending_count++;

  if (machine->freelist_head) {
    struct um_op *op = machine->freelist_head;
    machine->freelist_head = op->next;
    um_op_clear(op);
    return op;
  }

  struct um_op *op = malloc(sizeof(struct um_op));
  um_op_clear(op);
  return op;
}

inline void um_op_checkin(struct um *machine, struct um_op *op) {
  machine->pending_count--;

  op->next = machine->freelist_head;
  machine->freelist_head = op;
}

inline struct um_op *um_runqueue_find_by_fiber(struct um *machine, VALUE fiber) {
  struct um_op *op = machine->runqueue_head;
  while (op) {
    if (op->fiber == fiber) return op;

    op = op->next;
  }
  return NULL;
}

inline void um_runqueue_push(struct um *machine, struct um_op *op) {
  if (machine->runqueue_tail) {
    op->prev = machine->runqueue_tail;
    machine->runqueue_tail->next = op;
    machine->runqueue_tail = op;
  }
  else {
    op->prev = NULL;
    machine->runqueue_head = machine->runqueue_tail = op;
  }
  op->next = NULL;
}

inline void um_runqueue_unshift(struct um *machine, struct um_op *op) {
  if (machine->runqueue_head) {
    op->next = machine->runqueue_head;
    machine->runqueue_head->prev = op;
    machine->runqueue_head = op;
  }
  else {
    op->next = NULL;
    machine->runqueue_head = machine->runqueue_tail = op;
  }
  op->prev = NULL;
}

inline struct um_op *um_runqueue_shift(struct um *machine) {
  struct um_op *op = machine->runqueue_head;
  if (!op) return NULL;

  op->prev = NULL;
  if (!op->next) {
    machine->runqueue_head = machine->runqueue_tail = NULL;    
  }
  else {
    machine->runqueue_head = op->next;
    op->next = NULL;
  }
  return op;
}
