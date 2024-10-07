#include "um.h"

inline struct um_result_entry *um_result_checkout(struct um *machine) {
  if (machine->result_freelist) {
    struct um_result_entry *entry = machine->result_freelist;
    machine->result_freelist = entry->next;
    return entry;
  }

  struct um_result_entry *entry = malloc(sizeof(struct um_result_entry));
  return entry;
}

inline void um_result_checkin(struct um *machine, struct um_result_entry *entry) {
  entry->next = machine->result_freelist;
  machine->result_freelist = entry;
}

inline void um_op_result_cleanup(struct um *machine, struct um_op *op) {
  struct um_result_entry *entry = op->results_head;
  while (entry) {
    struct um_result_entry *next = entry->next;
    um_result_checkin(machine, entry);
    entry = next;
  }
  op->results_head = op->results_tail = NULL;
}

inline void um_op_result_push(struct um *machine, struct um_op *op, __s32 result, __u32 flags) {
  struct um_result_entry *entry = um_result_checkout(machine);
  entry->next = 0;
  entry->result = result;
  entry->flags = flags;
  if (op->results_tail) {
    op->results_tail->next = entry;
    op->results_tail = entry;
  }
  else {
    op->results_head = op->results_tail = entry;
  }
}

inline int um_op_result_shift(struct um *machine, struct um_op *op, __s32 *result, __u32 *flags) {
  if (!op->results_head) return 0;

  struct um_result_entry *entry = op->results_head;
  *result = entry->result;
  *flags = entry->flags;
  op->results_head = entry->next;
  if (!op->results_head)
    op->results_tail = NULL;
  um_result_checkin(machine, entry);
  return 1;
}

static inline void um_op_clear(struct um *machine, struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->resume_value, Qnil);
}

inline struct um_op *um_op_checkout(struct um *machine, enum op_kind kind) {
  machine->pending_count++;

  struct um_op *op = machine->op_freelist;
  if (op)
    machine->op_freelist = op->next;
  else
    op = malloc(sizeof(struct um_op));

  um_op_clear(machine, op);
  op->kind = kind;
  printf(">> checkout op %p kind %d\n", op, op->kind);
  // um_abandonned_add(machine, op);
  return op;
}

inline void um_op_checkin(struct um *machine, struct um_op *op) {
  machine->pending_count--;

  // um_abandonned_remove(machine, op);
  printf("<< checkin op %p kind %d\n", op, op->kind);
  um_op_result_cleanup(machine, op);
  op->next = machine->op_freelist;
  machine->op_freelist = op;
}

inline void um_free_op_linked_list(struct um *machine, struct um_op *op) {
  while (op) {
    struct um_op *next = op->next;
    um_op_result_cleanup(machine, op);
    free(op);
    op = next;
  }
}

inline void um_free_result_linked_list(struct um *machine, struct um_result_entry *entry) {
  while (entry) {
    struct um_result_entry *next = entry->next;
    free(entry);
    entry = next;
  }
}

inline void um_mark_op_linked_list(struct um_op *head) {
  while (head) {
    rb_gc_mark_movable(head->fiber);
    rb_gc_mark_movable(head->resume_value);
    head = head->next;
  }
}

inline void um_compact_op_linked_list(struct um_op *head) {
  while (head) {
    head->fiber = rb_gc_location(head->fiber);
    head->resume_value = rb_gc_location(head->resume_value);
    head = head->next;
  }
}

inline void um_abandonned_add(struct um *machine, struct um_op *op) {
  op->next = machine->cancelled_head;
  if (machine->cancelled_head)
    machine->cancelled_head->prev = op;
  machine->cancelled_head = op;
}

inline void um_abandonned_remove(struct um *machine, struct um_op *op) {
  if (machine->cancelled_head == op)
    machine->cancelled_head = op->next;
  if (op->next)
    op->next->prev = op->prev;
  if (op->prev)
    op->prev->next = op->next;
}
