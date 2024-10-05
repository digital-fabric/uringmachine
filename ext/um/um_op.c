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

inline void um_op_clear(struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  op->fiber = op->resume_value = Qnil;
}

inline struct um_op *um_op_checkout(struct um *machine) {
  machine->pending_count++;

  struct um_op *op = machine->op_freelist;
  if (op)
    machine->op_freelist = op->next;
  else
    op = malloc(sizeof(struct um_op));

  um_op_clear(op);
  return op;
}

inline void um_op_checkin(struct um *machine, struct um_op *op) {
  machine->pending_count--;

  um_op_result_cleanup(machine, op);
  op->next = machine->op_freelist;
  machine->op_freelist = op;
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
