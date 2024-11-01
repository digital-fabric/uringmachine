#include "um.h"

void um_op_clear(struct um *machine, struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->value, Qnil);
}

void um_op_transient_add(struct um *machine, struct um_op *op) {
  // printf("transient_add %p kind %d transient_head %p\n", op, op->kind, machine->transient_head);
  // INSPECT("  fiber", op->fiber);
  // INSPECT("  value", op->fiber);
  if (machine->transient_head) {
    op->transient_next = machine->transient_head;
    machine->transient_head->transient_prev = op;
  }
  machine->transient_head = op;
}

void um_op_transient_remove(struct um *machine, struct um_op *op) {
  // printf("transient_remove %p transient_head %p\n", op, machine->transient_head);
  // INSPECT("  fiber", op->fiber);
  // INSPECT("  value", op->fiber);
  // printf("  prev %p next %p\n", op->transient_prev, op->transient_next);
  if (op->transient_prev)
    op->transient_prev->transient_next = op->transient_next;
  if (op->transient_next)
    op->transient_next->transient_prev = op->transient_prev;

  if (machine->transient_head == op)
    machine->transient_head = op->transient_next;
}

void um_op_transient_mark(struct um *machine) {
  struct um_op *op = machine->transient_head;
  while (op) {
    printf("mark %p (prev %p next %p)\n", op, op->transient_prev, op->transient_next);
    struct um_op *next = op->transient_next;
    rb_gc_mark_movable(op->fiber);
    rb_gc_mark_movable(op->value);
    op = next;
  }
}

void um_op_transient_compact(struct um *machine) {
  struct um_op *op = machine->transient_head;
  while (op) {
    printf("compact %p (prev %p next %p)\n", op, op->transient_prev, op->transient_next);
    struct um_op *next = op->transient_next;
    op->fiber = rb_gc_location(op->fiber);
    op->value = rb_gc_location(op->value);
    op = next;
  }
}
