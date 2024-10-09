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
  struct um_result_entry *entry = op->list_results.head;
  while (entry) {
    struct um_result_entry *next = entry->next;
    um_result_checkin(machine, entry);
    entry = next;
  }
  op->list_results.head = op->list_results.tail = NULL;
}

inline void um_op_result_push(struct um *machine, struct um_op *op, __s32 result, __u32 flags) {
  struct um_result_entry *entry = um_result_checkout(machine);
  entry->next = 0;
  entry->result = result;
  entry->flags = flags;
  if (op->list_results.tail) {
    op->list_results.tail->next = entry;
    op->list_results.tail = entry;
  }
  else {
    op->list_results.head = op->list_results.tail = entry;
  }
}

inline void um_op_push_multishot_result(struct um *machine, struct um_op *op, __s32 result, __u32 flags) {
  um_op_result_push(machine, op, result, flags);
  if (!op->aux) {
    op->aux = um_op_idle_checkout(machine, OP_MULTISHOT_AUX);
    RB_OBJ_WRITE(machine->self, &op->aux->fiber, op->fiber);
    RB_OBJ_WRITE(machine->self, &op->aux->value, Qnil);
    // TODO: add parameter to um_op_idle_checkout to put object immediately in
    // completed queue.
    um_op_state_transition(machine, op->aux, OP_SCHEDULED);
    op->aux->flags |= OP_F_AUTO_CHECKIN;
  }
}

inline int um_op_result_shift(struct um *machine, struct um_op *op, __s32 *result, __u32 *flags) {
  if (!op->list_results.head) return 0;

  struct um_result_entry *entry = op->list_results.head;
  *result = entry->result;
  *flags = entry->flags;
  op->list_results.head = entry->next;
  if (!op->list_results.head)
    op->list_results.tail = NULL;
  um_result_checkin(machine, entry);
  return 1;
}

inline void um_op_result_list_free(struct um *machine) {
  struct um_result_entry *entry = machine->result_freelist;
  while (entry) {
    struct um_result_entry *next = entry->next;
    free(entry);
    entry = next;
  }
}

static inline void um_op_clear(struct um *machine, struct um_op *op) {
  memset(op, 0, sizeof(struct um_op));
  RB_OBJ_WRITE(machine->self, &op->fiber, Qnil);
  RB_OBJ_WRITE(machine->self, &op->value, Qnil);
}

struct um_op* um_op_idle_checkout(struct um *machine, enum op_kind kind) {
  struct um_op *op = um_op_list_shift(&machine->list_idle);
  if (!op)
    op = malloc(sizeof(struct um_op));

  um_op_clear(machine, op);
  op->kind = kind;

  um_op_state_transition(machine, op, OP_PENDING);
  // printf(">> checkout op %p kind %d\n", op, op->kind);
  machine->pending_count++;

  // um_abandonned_add(machine, op);
  return op;
}

inline void op_list_remove(struct um_op_linked_list *list, struct um_op *op) {
  if (op->prev) op->prev = op->next;
  if (op->next) op->next = op->prev;
  if (op == list->head) list->head = op->next;
  if (op == list->tail) list->tail = op->prev;
}

inline void op_list_add_tail(struct um_op_linked_list *list, struct um_op *op) {
  if (list->tail) {
    list->tail->next = op;
    op->prev = list->tail;
    list->tail = op;
  }
  else {
    op->prev = NULL;
    list->head = list->tail = op;
  }
  op->next = NULL;
}

inline struct um_op *um_op_list_shift(struct um_op_linked_list *list) {
  struct um_op *op = list->head;
  if (!op) return NULL;

  op->prev = NULL;
  if (!op->next) {
    list->head = list->tail = NULL;
  }
  else {
    list->head = op->next;
    op->next = NULL;
  }
  return op;
}

inline struct um_op_linked_list *op_list_by_state(struct um *machine, enum op_state state) {
  switch (state) {
    case OP_IDLE:       return &machine->list_idle;
    case OP_PENDING:    return &machine->list_pending;
    case OP_SCHEDULED:  return &machine->list_scheduled;
    default:            return NULL;
  }
}

inline void um_op_state_transition(struct um *machine, struct um_op *op, enum op_state new_state) {
  if (new_state == op->state) return;

  if (op->state != OP_DONE)
    op_list_remove(op_list_by_state(machine, op->state), op);
  
  op->state = new_state;
  if (new_state == OP_IDLE) {
    // printf("<< checkin op %p kind %d\n", op, op->kind);
    machine->pending_count--;
    if (op->flags & OP_F_MULTISHOT)
      um_op_result_cleanup(machine, op);
  }
  
  if (new_state != OP_DONE)
    op_list_add_tail(op_list_by_state(machine, op->state), op);
}

inline void op_free(struct um *machine, struct um_op *op) {
  if (op->flags & OP_F_MULTISHOT)
    um_op_result_cleanup(machine, op);
  free(op);
}

inline void um_op_free_list(struct um *machine, struct um_op_linked_list *list) {
  struct um_op *op = list->head;
  while (op) {
    struct um_op *next = op->next;
    op_free(machine, op);
    op = next;
  }
}

inline struct um_op *um_op_search_by_fiber(struct um_op_linked_list *list, VALUE fiber) {
  struct um_op *op = list->head;
  while (op) {
    if (op->fiber == fiber) return op;
    op = op->next;
  }
  return NULL;
}

inline void um_mark_op_linked_list(struct um_op_linked_list *list) {
  struct um_op *op = list->head;
  while (op) {
    rb_gc_mark_movable(op->fiber);
    rb_gc_mark_movable(op->value);
    op = op->next;
  }
}

inline void um_compact_op_linked_list(struct um_op_linked_list *list) {
  struct um_op *op = list->head;
  while (op) {
    op->fiber = rb_gc_location(op->fiber);
    op->value = rb_gc_location(op->value);
    op = op->next;
  }
}
