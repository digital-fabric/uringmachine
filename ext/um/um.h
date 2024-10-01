#ifndef UM_H
#define UM_H

#include "ruby.h"
#include <liburing.h>

// debugging
#define OBJ_ID(obj) (NUM2LONG(rb_funcall(obj, rb_intern("object_id"), 0)))
#define INSPECT(str, obj) { printf(str); VALUE s = rb_funcall(obj, rb_intern("inspect"), 0); printf(": %s\n", StringValueCStr(s)); }
#define CALLER() rb_funcall(rb_mKernel, rb_intern("caller"), 0)
#define TRACE_CALLER() INSPECT("caller: ", CALLER())
#define TRACE_FREE(ptr) //printf("Free %p %s:%d\n", ptr, __FILE__, __LINE__)

// branching
#ifndef unlikely
#define unlikely(cond)	__builtin_expect(!!(cond), 0)
#endif

#ifndef likely
#define likely(cond)	__builtin_expect(!!(cond), 1)
#endif

enum op_state {
  OP_initial,
  OP_submitted,
  OP_completed,
  OP_cancelled,
  OP_abandonned
};

struct um_op {
  enum op_state state;
  struct um_op *prev;
  struct um_op *next;
  
  VALUE fiber;
  VALUE resume_value;
  int cqe_result;
  int cqe_flags;
};

struct buf_ring_descriptor {
  struct io_uring_buf_ring *br;
  size_t br_size;
  // struct io_uring_buf_ring *buf_ring;
  unsigned buf_count;
  unsigned buf_size;
	char *buf_base;
  // size_t buf_ring_size;
};

#define BUFFER_RING_MAX_COUNT 10

struct um {
  struct um_op *freelist_head;
  struct um_op *runqueue_head;
  struct um_op *runqueue_tail;

  struct io_uring ring;

  unsigned int    ring_initialized;
  unsigned int    unsubmitted_count;

  struct buf_ring_descriptor buffer_rings[BUFFER_RING_MAX_COUNT];
  unsigned int buffer_ring_count;
};

struct __kernel_timespec um_double_to_timespec(double value);

void um_cleanup(struct um *machine);

void um_free_linked_list(struct um_op *op);
void um_fiber_switch(struct um *machine);

void um_op_checkin(struct um *machine, struct um_op *op);
struct um_op* um_op_checkout(struct um *machine);

VALUE um_raise_exception(VALUE v);

struct um_op *um_runqueue_find_by_fiber(struct um *machine, VALUE fiber);
void um_runqueue_push(struct um *machine, struct um_op *op);
struct um_op *um_runqueue_shift(struct um *machine);

int um_value_is_exception_p(VALUE v);




VALUE um_sleep(struct um *machine, double duration);

#endif // UM_H
