#include "um.h"

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
  um_cleanup((struct um *)ptr);
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

  machine->ring_initialized = 0;
  machine->unsubmitted_count = 0;
  machine->buffer_ring_count = 0;
  machine->runqueue_head = NULL;
  machine->runqueue_tail = NULL;
  machine->freelist_head = NULL;

  unsigned prepared_limit = 4096;
  int flags = 0;
  #ifdef HAVE_IORING_SETUP_SUBMIT_ALL
  flags |= IORING_SETUP_SUBMIT_ALL;
  #endif
  #ifdef HAVE_IORING_SETUP_COOP_TASKRUN
  flags |= IORING_SETUP_COOP_TASKRUN;
  #endif

  while (1) {
    int ret = io_uring_queue_init(prepared_limit, &machine->ring, flags);
    if (likely(!ret)) break;

    // if ENOMEM is returned, try with half as much entries
    if (unlikely(ret == -ENOMEM && prepared_limit > 64))
      prepared_limit = prepared_limit / 2;
    else
      rb_syserr_fail(-ret, strerror(-ret));
  }
  machine->ring_initialized = 1;

  return self;
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

VALUE UM_sleep(VALUE self, VALUE duration) {
  struct um *machine = get_machine(self);
  um_sleep(machine, NUM2DBL(duration));
  return duration;
}

void Init_UM(void) {
  rb_ext_ractor_safe(true);

  cUM = rb_define_class("UringMachine", rb_cObject);
  rb_define_alloc_func(cUM, UM_allocate);

  rb_define_method(cUM, "initialize", UM_initialize, 0);
  // rb_define_method(cUM, "setup_buffer_ring", UM_setup_buffer_ring, 1);

  rb_define_method(cUM, "snooze", UM_snooze, 0);
  rb_define_method(cUM, "yield", UM_yield, 0);
  rb_define_method(cUM, "schedule", UM_schedule, 2);
  rb_define_method(cUM, "interrupt", UM_interrupt, 2);

  rb_define_method(cUM, "sleep", UM_sleep, 1);

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
