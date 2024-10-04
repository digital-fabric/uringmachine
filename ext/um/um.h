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
  OP_initial,     // initial state
  OP_submitted,   // op has been submitted (SQE prepared)
  OP_completed,   // op has been completed (CQE processed)
  OP_cancelled,   // op is cancelled (after CQE is processed)
  OP_abandonned,  // op is abandonned by fiber (before CQE is processed)
  OP_schedule,    // the corresponding fiber is scheduled
};

struct um_result_entry {
  struct um_result_entry *next;

  int result;
  int flags;
};

struct um_op {
  enum op_state state;
  struct um_op *prev;
  struct um_op *next;

  // linked list for multishot results
  struct um_result_entry *results_head;
  struct um_result_entry *results_tail;
  
  VALUE fiber;
  VALUE resume_value;
  int is_multishot;
  struct __kernel_timespec ts;
  
  int cqe_result;
  int cqe_flags;
};

struct buf_ring_descriptor {
  struct io_uring_buf_ring *br;
  size_t br_size;
  unsigned buf_count;
  unsigned buf_size;
	char *buf_base;
};

#define BUFFER_RING_MAX_COUNT 10

struct um {
  struct um_op *op_freelist;
  struct um_result_entry *result_freelist;

  struct um_op *runqueue_head;
  struct um_op *runqueue_tail;

  struct io_uring ring;

  unsigned int    ring_initialized;
  unsigned int    unsubmitted_count;
  unsigned int    pending_count;

  struct buf_ring_descriptor buffer_rings[BUFFER_RING_MAX_COUNT];
  unsigned int buffer_ring_count;
};

extern VALUE cUM;

void um_setup(struct um *machine);
void um_teardown(struct um *machine);
void um_free_op_linked_list(struct um *machine, struct um_op *op);
void um_free_result_linked_list(struct um *machine, struct um_result_entry *entry);

struct __kernel_timespec um_double_to_timespec(double value);
int um_value_is_exception_p(VALUE v);
VALUE um_raise_exception(VALUE v);
void um_raise_on_system_error(int result);

void * um_prepare_read_buffer(VALUE buffer, unsigned len, int ofs);
void um_update_read_buffer(struct um *machine, VALUE buffer, int buffer_offset, int result, int flags);
VALUE get_string_from_buffer_ring(struct um *machine, int bgid, int result, int flags);

VALUE um_fiber_switch(struct um *machine);
VALUE um_await(struct um *machine);

void um_op_checkin(struct um *machine, struct um_op *op);
struct um_op* um_op_checkout(struct um *machine);
void um_op_result_push(struct um *machine, struct um_op *op, int result, int flags);
int um_op_result_shift(struct um *machine, struct um_op *op, int *result, int *flags);

struct um_op *um_runqueue_find_by_fiber(struct um *machine, VALUE fiber);
void um_runqueue_push(struct um *machine, struct um_op *op);
struct um_op *um_runqueue_shift(struct um *machine);
void um_runqueue_unshift(struct um *machine, struct um_op *op);

void um_schedule(struct um *machine, VALUE fiber, VALUE value);
void um_interrupt(struct um *machine, VALUE fiber, VALUE value);
VALUE um_timeout(struct um *machine, VALUE interval, VALUE class);

VALUE um_sleep(struct um *machine, double duration);
VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset);
VALUE um_read_each(struct um *machine, int fd, int bgid);
VALUE um_write(struct um *machine, int fd, VALUE buffer, int len);
VALUE um_close(struct um *machine, int fd);

VALUE um_accept(struct um *machine, int fd);
VALUE um_accept_each(struct um *machine, int fd);
VALUE um_socket(struct um *machine, int domain, int type, int protocol, uint flags);
VALUE um_connect(struct um *machine, int fd, const struct sockaddr *addr, socklen_t addrlen);
VALUE um_send(struct um *machine, int fd, VALUE buffer, int len, int flags);
VALUE um_recv(struct um *machine, int fd, VALUE buffer, int maxlen, int flags);

#endif // UM_H
