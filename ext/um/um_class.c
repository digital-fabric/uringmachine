#include "um.h"
#include <sys/mman.h>

VALUE cUM;

static void UM_mark(void *ptr) {
  // struct um *machine = ptr;
  // um_runqueue_mark(machine);
}

static void UM_compact(void *ptr) {
  // struct um *machine = ptr;
  // um_runqueue_compact(machine);
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
  um_setup(machine);
  return self;
}

VALUE UM_setup_buffer_ring(VALUE self, VALUE size, VALUE count) {
  struct um *machine = get_machine(self);

  if (machine->buffer_ring_count == BUFFER_RING_MAX_COUNT)
    rb_raise(rb_eRuntimeError, "Cannot setup more than BUFFER_RING_MAX_COUNT buffer rings");

  struct buf_ring_descriptor *desc = machine->buffer_rings + machine->buffer_ring_count;
  desc->buf_count = NUM2UINT(count);
  desc->buf_size = NUM2UINT(size);

  desc->br_size = sizeof(struct io_uring_buf) * desc->buf_count;
	void *mapped = mmap(
    NULL, desc->br_size, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, 0, 0
  );
  if (mapped == MAP_FAILED)
    rb_raise(rb_eRuntimeError, "Failed to allocate buffer ring");

  desc->br = (struct io_uring_buf_ring *)mapped;
  io_uring_buf_ring_init(desc->br);

  unsigned bg_id = machine->buffer_ring_count;
  struct io_uring_buf_reg reg = {
    .ring_addr = (unsigned long)desc->br,
		.ring_entries = desc->buf_count,
		.bgid = bg_id
  };
	int ret = io_uring_register_buf_ring(&machine->ring, &reg, 0);
	if (ret) {
    munmap(desc->br, desc->br_size);
    rb_syserr_fail(-ret, strerror(-ret));
	}

  desc->buf_base = malloc(desc->buf_count * desc->buf_size);
  if (!desc->buf_base) {
    io_uring_free_buf_ring(&machine->ring, desc->br, desc->buf_count, bg_id);
    rb_raise(rb_eRuntimeError, "Failed to allocate buffers");
  }

  int mask = io_uring_buf_ring_mask(desc->buf_count);
	for (unsigned i = 0; i < desc->buf_count; i++) {
		io_uring_buf_ring_add(
      desc->br, desc->buf_base + i * desc->buf_size, desc->buf_size,
      i, mask, i);
	}
	io_uring_buf_ring_advance(desc->br, desc->buf_count);
  machine->buffer_ring_count++;
  return UINT2NUM(bg_id);
}

VALUE UM_pending_count(VALUE self) {
  struct um *machine = get_machine(self);
  return INT2FIX(machine->pending_count);
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

VALUE UM_accept(VALUE self, VALUE fd) {
  struct um *machine = get_machine(self);
  return um_accept(machine, NUM2INT(fd));
}

VALUE UM_accept_each(VALUE self, VALUE fd) {
  struct um *machine = get_machine(self);
  return um_accept_each(machine, NUM2INT(fd));
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

  rb_define_method(cUM, "accept", UM_accept, 1);
  rb_define_method(cUM, "accept_each", UM_accept_each, 1);

  // rb_define_method(cUM, "emit", UM_emit, 1);

  // rb_define_method(cUM, "prep_accept", UM_prep_accept, 1);
  // rb_define_method(cUM, "prep_cancel", UM_prep_cancel, 1);
  // rb_define_method(cUM, "prep_close", UM_prep_close, 1);
  // rb_define_method(cUM, "prep_nop", UM_prep_nop, 0);
  // rb_define_method(cUM, "prep_read", UM_prep_read, 1);
  // rb_define_method(cUM, "prep_timeout", UM_prep_timeout, 1);
  // rb_define_method(cUM, "prep_write", UM_prep_write, 1);

  // rb_define_method(cUM, "submit", UM_submit, 0);
  // rb_define_method(cUM, "wait_for_completion", UM_wait_for_completion, 0);
  // rb_define_method(cUM, "process_completions", UM_process_completions, -1);
  // rb_define_method(cUM, "process_completions_loop", UM_process_completions_loop, 0);

  // SYM_accept        = MAKE_SYM("accept");
  // SYM_block         = MAKE_SYM("block");
  // SYM_buffer        = MAKE_SYM("buffer");
  // SYM_buffer_group  = MAKE_SYM("buffer_group");
  // SYM_buffer_offset = MAKE_SYM("buffer_offset");
  // SYM_close         = MAKE_SYM("close");
  // SYM_count         = MAKE_SYM("count");
  // SYM_emit          = MAKE_SYM("emit");
  // SYM_fd            = MAKE_SYM("fd");
  // SYM_id            = MAKE_SYM("id");
  // SYM_interval      = MAKE_SYM("interval");
  // SYM_len           = MAKE_SYM("len");
  // SYM_link          = MAKE_SYM("link");
  // SYM_multishot     = MAKE_SYM("multishot");
  // SYM_op            = MAKE_SYM("op");
  // SYM_read          = MAKE_SYM("read");
  // SYM_result        = MAKE_SYM("result");
  // SYM_signal        = MAKE_SYM("signal");
  // SYM_size          = MAKE_SYM("size");
  // SYM_spec_data     = MAKE_SYM("spec_data");
  // SYM_stop          = MAKE_SYM("stop");
  // SYM_timeout       = MAKE_SYM("timeout");
  // SYM_utf8          = MAKE_SYM("utf8");
  // SYM_write         = MAKE_SYM("write");
}
