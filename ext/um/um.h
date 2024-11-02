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

enum op_kind {
  OP_TIMEOUT,
  OP_SCHEDULE,
  OP_MULTISHOT_AUX,
  OP_SLEEP,
  OP_READ,
  OP_READ_MULTISHOT,
  OP_WRITE,
  OP_CLOSE,
  OP_ACCEPT,
  OP_ACCEPT_MULTISHOT,
  OP_RECV,
  OP_RECV_MULTISHOT,
  OP_SEND,
  OP_SOCKET,
  OP_CONNECT,
  OP_BIND,
  OP_LISTEN,
  OP_GETSOCKOPT,
  OP_SETSOCKOPT,
  OP_FUTEX_WAIT,
  OP_FUTEX_WAKE
};

#define OP_F_COMPLETED        (1U << 0)
#define OP_F_TRANSIENT        (1U << 1)
#define OP_F_IGNORE_CANCELED  (1U << 2)

struct um_op {
  enum op_kind kind;
  unsigned flags;

  VALUE fiber;
  VALUE value;

  __s32 cqe_res;
  __u32 cqe_flags;

  struct um_op *transient_prev;
  struct um_op *transient_next;

  struct __kernel_timespec ts; // used for timeout operation
};

struct um_buffer {
  struct um_buffer *next;
  void *ptr;
  long len;
};

struct buf_ring_descriptor {
  struct io_uring_buf_ring *br;
  size_t br_size;
  unsigned buf_count;
  unsigned buf_size;
  unsigned buf_mask;
	void *buf_base;
};

#define BUFFER_RING_MAX_COUNT 10

struct um {
  VALUE self;
  VALUE poll_fiber;

  struct um_buffer *buffer_freelist;

  struct io_uring ring;

  unsigned int    ring_initialized;
  unsigned int    unsubmitted_count;
  unsigned int    pending_count;

  struct buf_ring_descriptor buffer_rings[BUFFER_RING_MAX_COUNT];
  unsigned int buffer_ring_count;

  struct um_op *transient_head;
};

struct um_mutex {
  VALUE self;
  uint32_t state;
};

struct um_queue_entry {
  struct um_queue_entry *prev;
  struct um_queue_entry *next;
  VALUE value;
};

struct um_queue {
  VALUE self;

  struct um_queue_entry *head;
  struct um_queue_entry *tail;
  struct um_queue_entry *free_head;
  
  uint32_t num_waiters;
  uint32_t state;
  uint32_t count;
};

extern VALUE cUM;
extern VALUE cMutex;
extern VALUE cQueue;

void um_setup(VALUE self, struct um *machine);
void um_teardown(struct um *machine);

void um_op_clear(struct um *machine, struct um_op *op);
void um_op_transient_add(struct um *machine, struct um_op *op);
void um_op_transient_remove(struct um *machine, struct um_op *op);
void um_op_transient_mark(struct um *machine);
void um_op_transient_compact(struct um *machine);

struct um_buffer *um_buffer_checkout(struct um *machine, int len);
void um_buffer_checkin(struct um *machine, struct um_buffer *buffer);
void um_free_buffer_linked_list(struct um *machine);

struct __kernel_timespec um_double_to_timespec(double value);
int um_value_is_exception_p(VALUE v);
VALUE um_raise_exception(VALUE v);

#define raise_if_exception(v) (um_value_is_exception_p(v) ? um_raise_exception(v) : v)

void um_prep_op(struct um *machine, struct um_op *op, enum op_kind kind);
void um_raise_on_error_result(int result);
void * um_prepare_read_buffer(VALUE buffer, unsigned len, int ofs);
void um_update_read_buffer(struct um *machine, VALUE buffer, int buffer_offset, __s32 result, __u32 flags);
int um_setup_buffer_ring(struct um *machine, unsigned size, unsigned count);
VALUE um_get_string_from_buffer_ring(struct um *machine, int bgid, __s32 result, __u32 flags);

struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op);

VALUE um_fiber_switch(struct um *machine);
VALUE um_await(struct um *machine);
void um_cancel_and_wait(struct um *machine, struct um_op *op);
int um_check_completion(struct um *machine, struct um_op *op);

#define um_op_completed_p(op) ((op)->flags & OP_F_COMPLETED)

void um_schedule(struct um *machine, VALUE fiber, VALUE value);
void um_interrupt(struct um *machine, VALUE fiber, VALUE value);
VALUE um_timeout(struct um *machine, VALUE interval, VALUE class);

VALUE um_sleep(struct um *machine, double duration);
VALUE um_read(struct um *machine, int fd, VALUE buffer, int maxlen, int buffer_offset);
VALUE um_read_each(struct um *machine, int fd, int bgid);
VALUE um_write(struct um *machine, int fd, VALUE str, int len);
VALUE um_close(struct um *machine, int fd);

VALUE um_accept(struct um *machine, int fd);
VALUE um_accept_each(struct um *machine, int fd);
VALUE um_socket(struct um *machine, int domain, int type, int protocol, uint flags);
VALUE um_connect(struct um *machine, int fd, const struct sockaddr *addr, socklen_t addrlen);
VALUE um_send(struct um *machine, int fd, VALUE buffer, int len, int flags);
VALUE um_recv(struct um *machine, int fd, VALUE buffer, int maxlen, int flags);
VALUE um_recv_each(struct um *machine, int fd, int bgid, int flags);
VALUE um_bind(struct um *machine, int fd, struct sockaddr *addr, socklen_t addrlen);
VALUE um_listen(struct um *machine, int fd, int backlog);
VALUE um_getsockopt(struct um *machine, int fd, int level, int opt);
VALUE um_setsockopt(struct um *machine, int fd, int level, int opt, int value);

struct um_mutex *Mutex_data(VALUE self);
struct um_queue *Queue_data(VALUE self);

void um_mutex_init(struct um_mutex *mutex);
VALUE um_mutex_synchronize(struct um *machine, uint32_t *state);

void um_queue_init(struct um_queue *queue);
void um_queue_free(struct um_queue *queue);
void um_queue_mark(struct um_queue *queue);
void um_queue_compact(struct um_queue *queue);
VALUE um_queue_push(struct um *machine, struct um_queue *queue, VALUE value);
VALUE um_queue_pop(struct um *machine, struct um_queue *queue);
VALUE um_queue_unshift(struct um *machine, struct um_queue *queue, VALUE value);
VALUE um_queue_shift(struct um *machine, struct um_queue *queue);

void um_define_net_constants(VALUE mod);

#endif // UM_H
