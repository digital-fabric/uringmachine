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
  OP_IDLE,
  OP_PENDING,
  OP_SCHEDULED,
  OP_DONE
};

enum op_kind {
  OP_TIMEOUT,
  OP_SCHEDULE,
  OP_INTERRUPT,
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
  OP_LISTEN
};

struct um_result_entry {
  struct um_result_entry *next;

  __s32 result;
  __u32 flags;
};

struct um_result_list {
  struct um_result_entry *head;
  struct um_result_entry *tail;
};

#define OP_F_MULTISHOT    (1U << 0)
#define OP_F_CANCELLED    (1U << 1)
#define OP_F_DISCARD      (1U << 2)
#define OP_F_AUTO_CHECKIN (1U << 3)

struct um_op {
  enum op_kind kind;
  enum op_state state;
  unsigned flags;

  struct um_op *prev;
  struct um_op *next;
  
  union {
    struct __kernel_timespec ts;
    struct {
      struct um_op *aux;
      struct um_result_list list_results;
    };
    struct um_result_entry cqe;
  };

  VALUE fiber;
  VALUE value;
};

struct um_op_linked_list {
  struct um_op *head;
  struct um_op *tail;
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

  struct um_op_linked_list list_idle;
  struct um_op_linked_list list_pending;
  struct um_op_linked_list list_scheduled;

  struct um_result_entry *result_freelist;
  struct um_buffer *buffer_freelist;

  struct io_uring ring;

  unsigned int    ring_initialized;
  unsigned int    unsubmitted_count;
  unsigned int    pending_count;

  struct buf_ring_descriptor buffer_rings[BUFFER_RING_MAX_COUNT];
  unsigned int buffer_ring_count;
};

extern VALUE cUM;

void um_setup(VALUE self, struct um *machine);
void um_teardown(struct um *machine);

struct um_op*  um_op_idle_checkout(struct um *machine, enum op_kind kind);
void um_op_state_transition(struct um *machine, struct um_op *op, enum op_state new_state);
void um_op_push_multishot_result(struct um *machine, struct um_op *op, __s32 result, __u32 flags);
struct um_op *um_op_list_shift(struct um_op_linked_list *list);
struct um_op *um_op_search_by_fiber(struct um_op_linked_list *list, VALUE fiber);

void um_op_free_list(struct um *machine, struct um_op_linked_list *list);
void um_mark_op_linked_list(struct um_op_linked_list *list);
void um_compact_op_linked_list(struct um_op_linked_list *list);

void um_op_result_push(struct um *machine, struct um_op *op, __s32 result, __u32 flags);
int um_op_result_shift(struct um *machine, struct um_op *op, __s32 *result, __u32 *flags);
void um_op_result_cleanup(struct um *machine, struct um_op *op);
void um_op_result_list_free(struct um *machine);

struct um_buffer *um_buffer_checkout(struct um *machine, int len);
void um_buffer_checkin(struct um *machine, struct um_buffer *buffer);
void um_free_buffer_linked_list(struct um *machine);

struct __kernel_timespec um_double_to_timespec(double value);
int um_value_is_exception_p(VALUE v);
VALUE um_raise_exception(VALUE v);
void um_raise_on_error_result(int result);
void * um_prepare_read_buffer(VALUE buffer, unsigned len, int ofs);
void um_update_read_buffer(struct um *machine, VALUE buffer, int buffer_offset, __s32 result, __u32 flags);
int um_setup_buffer_ring(struct um *machine, unsigned size, unsigned count);
VALUE um_get_string_from_buffer_ring(struct um *machine, int bgid, __s32 result, __u32 flags);

VALUE um_await(struct um *machine);
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
VALUE um_debug(struct um *machine);

void um_define_net_constants(VALUE mod);

#endif // UM_H
