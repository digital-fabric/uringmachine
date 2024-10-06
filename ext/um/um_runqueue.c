#include "um.h"

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

inline void um_runqueue_delete(struct um *machine, struct um_op *op) {
  struct um_op *prev = op->prev;
  struct um_op *next = op->next;
  if (prev) prev->next = next;
  if (next) next->prev = prev;
  if (machine->runqueue_head == op)
    machine->runqueue_head = next;
  if (machine->runqueue_tail == op)
    machine->runqueue_tail = prev;
}

inline void um_runqueue_mark(struct um *machine) {
  struct um_op *op = machine->runqueue_head;
  while (op) {
    rb_gc_mark_movable(op->fiber);
    rb_gc_mark_movable(op->resume_value);
    op = op->next;
  }
}

inline void um_runqueue_compact(struct um *machine) {
  struct um_op *op = machine->runqueue_head;
  while (op) {
    op->fiber = rb_gc_location(op->fiber);
    op->resume_value = rb_gc_location(op->resume_value);
    op = op->next;
  }
}
