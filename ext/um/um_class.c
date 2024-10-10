#include "um.h"
#include <arpa/inet.h>

VALUE cUM;

static void UM_mark(void *ptr) {
  struct um *machine = ptr;
  um_mark_op_linked_list(&machine->list_pending);
  um_mark_op_linked_list(&machine->list_scheduled);
}

static void UM_compact(void *ptr) {
  struct um *machine = ptr;
  um_compact_op_linked_list(&machine->list_pending);
  um_compact_op_linked_list(&machine->list_scheduled);
}

static void UM_free(void *ptr) {
  um_teardown((struct um *)ptr);
  free(ptr);
}

static size_t UM_size(const void *ptr) {
  return sizeof(struct um);
}

static const rb_data_type_t UM_type = {
    "UringMachine",
    {UM_mark, UM_free, UM_size, UM_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE UM_allocate(VALUE klass) {
  struct um *machine = ALLOC(struct um);

  return TypedData_Wrap_Struct(klass, &UM_type, machine);
}

inline struct um *get_machine(VALUE self) {
  struct um *machine = RTYPEDDATA_DATA(self);
  if (!machine->ring_initialized)
    rb_raise(rb_eRuntimeError, "Machine not initialized");
  return machine;
}

VALUE UM_initialize(VALUE self) {
  struct um *machine = RTYPEDDATA_DATA(self);
  um_setup(self, machine);
  return self;
}

VALUE UM_setup_buffer_ring(VALUE self, VALUE size, VALUE count) {
  struct um *machine = get_machine(self);
  int bgid = um_setup_buffer_ring(machine, NUM2UINT(size), NUM2UINT(count));
  return INT2NUM(bgid);
}

VALUE UM_pending_count(VALUE self) {
  struct um *machine = get_machine(self);
  return INT2NUM(machine->pending_count);
}

VALUE UM_snooze(VALUE self) {
  struct um *machine = get_machine(self);
  um_schedule(machine, rb_fiber_current(), Qnil);
  return um_await(machine);
}

VALUE UM_yield(VALUE self) {
  struct um *machine = get_machine(self);
  return um_await(machine);
}

VALUE UM_schedule(VALUE self, VALUE fiber, VALUE value) {
  struct um *machine = get_machine(self);
  um_schedule(machine, fiber, value);
  return self;
}

VALUE UM_interrupt(VALUE self, VALUE fiber, VALUE value) {
  struct um *machine = get_machine(self);
  um_interrupt(machine, fiber, value);
  return self;
}

VALUE UM_timeout(VALUE self, VALUE interval, VALUE class) {
  struct um *machine = get_machine(self);
  return um_timeout(machine, interval, class);
}

VALUE UM_sleep(VALUE self, VALUE duration) {
  struct um *machine = get_machine(self);
  um_sleep(machine, NUM2DBL(duration));
  return duration;
}

VALUE UM_read(int argc, VALUE *argv, VALUE self) {
  struct um *machine = get_machine(self);
  VALUE fd;
  VALUE buffer;
  VALUE maxlen;
  VALUE buffer_offset;
  rb_scan_args(argc, argv, "31", &fd, &buffer, &maxlen, &buffer_offset);

  return um_read(
    machine, NUM2INT(fd), buffer, NUM2INT(maxlen),
    NIL_P(buffer_offset) ? 0 : NUM2INT(buffer_offset)
  );
}

VALUE UM_read_each(VALUE self, VALUE fd, VALUE bgid) {
  struct um *machine = get_machine(self);
  return um_read_each(machine, NUM2INT(fd), NUM2INT(bgid));
}

VALUE UM_write(int argc, VALUE *argv, VALUE self) {
  struct um *machine = get_machine(self);
  VALUE fd;
  VALUE buffer;
  VALUE len;
  rb_scan_args(argc, argv, "21", &fd, &buffer, &len);

  int bytes = NIL_P(len) ? RSTRING_LEN(buffer) : NUM2INT(len);
  return um_write(machine, NUM2INT(fd), buffer, bytes);
}

VALUE UM_close(VALUE self, VALUE fd) {
  struct um *machine = get_machine(self);
  return um_close(machine, NUM2INT(fd));
}

VALUE UM_accept(VALUE self, VALUE fd) {
  struct um *machine = get_machine(self);
  return um_accept(machine, NUM2INT(fd));
}

VALUE UM_accept_each(VALUE self, VALUE fd) {
  struct um *machine = get_machine(self);
  return um_accept_each(machine, NUM2INT(fd));
}

VALUE UM_socket(VALUE self, VALUE domain, VALUE type, VALUE protocol, VALUE flags) {
  struct um *machine = get_machine(self);
  return um_socket(machine, NUM2INT(domain), NUM2INT(type), NUM2INT(protocol), NUM2UINT(flags));
}

VALUE UM_connect(VALUE self, VALUE fd, VALUE host, VALUE port) {
  struct um *machine = get_machine(self);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(StringValueCStr(host));
  addr.sin_port = htons(NUM2INT(port));

  return um_connect(machine, NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
}

VALUE UM_send(VALUE self, VALUE fd, VALUE buffer, VALUE len, VALUE flags) {
  struct um *machine = get_machine(self);
  return um_send(machine, NUM2INT(fd), buffer, NUM2INT(len), NUM2INT(flags));
}

VALUE UM_recv(VALUE self, VALUE fd, VALUE buffer, VALUE maxlen, VALUE flags) {
  struct um *machine = get_machine(self);
  return um_recv(machine, NUM2INT(fd), buffer, NUM2INT(maxlen), NUM2INT(flags));
}

VALUE UM_recv_each(VALUE self, VALUE fd, VALUE bgid, VALUE flags) {
  struct um *machine = get_machine(self);
  return um_recv_each(machine, NUM2INT(fd), NUM2INT(bgid), NUM2INT(flags));
}

VALUE UM_bind(VALUE self, VALUE fd, VALUE host, VALUE port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(StringValueCStr(host));
  addr.sin_port = htons(NUM2INT(port));

#ifdef HAVE_IO_URING_PREP_BIND
  struct um *machine = get_machine(self);
  return um_bind(machine, NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
#else
  int res = bind(NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  return INT2NUM(0);
#endif
}

VALUE UM_listen(VALUE self, VALUE fd, VALUE backlog) {
#ifdef HAVE_IO_URING_PREP_LISTEN
  struct um *machine = get_machine(self);
  return um_listen(machine, NUM2INT(fd), NUM2INT(backlog));
#else
  int res = listen(NUM2INT(fd), NUM2INT(backlog));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  return INT2NUM(0);
#endif
}

static inline int numeric_value(VALUE value) {
  switch (TYPE(value)) {
    case T_TRUE:
      return 1;
    case T_FALSE:
      return 0;
    default:
      return NUM2INT(value);
  }
}

VALUE UM_getsockopt(VALUE self, VALUE fd, VALUE level, VALUE opt) {
  struct um *machine = get_machine(self);
  return um_getsockopt(machine, NUM2INT(fd), NUM2INT(level), NUM2INT(opt));
}

VALUE UM_setsockopt(VALUE self, VALUE fd, VALUE level, VALUE opt, VALUE value) {
  struct um *machine = get_machine(self);
  return um_setsockopt(machine, NUM2INT(fd), NUM2INT(level), NUM2INT(opt), numeric_value(value));
}

VALUE UM_debug(VALUE self) {
  struct um *machine = get_machine(self);
  return um_debug(machine);
}

void Init_UM(void) {
  rb_ext_ractor_safe(true);

  cUM = rb_define_class("UringMachine", rb_cObject);
  rb_define_alloc_func(cUM, UM_allocate);

  rb_define_method(cUM, "initialize", UM_initialize, 0);
  rb_define_method(cUM, "setup_buffer_ring", UM_setup_buffer_ring, 2);
  rb_define_method(cUM, "pending_count", UM_pending_count, 0);

  rb_define_method(cUM, "snooze", UM_snooze, 0);
  rb_define_method(cUM, "yield", UM_yield, 0);
  rb_define_method(cUM, "schedule", UM_schedule, 2);
  rb_define_method(cUM, "interrupt", UM_interrupt, 2);
  rb_define_method(cUM, "timeout", UM_timeout, 2);

  rb_define_method(cUM, "sleep", UM_sleep, 1);
  rb_define_method(cUM, "read", UM_read, -1);
  rb_define_method(cUM, "read_each", UM_read_each, 2);
  rb_define_method(cUM, "write", UM_write, -1);
  rb_define_method(cUM, "close", UM_close, 1);

  rb_define_method(cUM, "accept", UM_accept, 1);
  rb_define_method(cUM, "accept_each", UM_accept_each, 1);
  rb_define_method(cUM, "socket", UM_socket, 4);
  rb_define_method(cUM, "connect", UM_connect, 3);
  rb_define_method(cUM, "send", UM_send, 4);
  rb_define_method(cUM, "recv", UM_recv, 4);
  rb_define_method(cUM, "recv_each", UM_recv_each, 3);
  rb_define_method(cUM, "bind", UM_bind, 3);
  rb_define_method(cUM, "listen", UM_listen, 2);
  rb_define_method(cUM, "getsockopt", UM_getsockopt, 3);
  rb_define_method(cUM, "setsockopt", UM_setsockopt, 4);

  rb_define_method(cUM, "debug", UM_debug, 0);

  um_define_net_constants(cUM);
}
