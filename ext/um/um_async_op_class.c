#include "um.h"
#include <stdlib.h>

VALUE cAsyncOp;

VALUE SYM_timeout;

static void AsyncOp_mark(void *ptr) {
  struct um_async_op *async_op = ptr;
  rb_gc_mark_movable(async_op->self);
  rb_gc_mark_movable(async_op->machine->self);
}

static void AsyncOp_compact(void *ptr) {
  struct um_async_op *async_op = ptr;
  async_op->self = rb_gc_location(async_op->self);
}

static size_t AsyncOp_size(const void *ptr) {
  return sizeof(struct um_async_op);
}

static void AsyncOp_free(void *ptr) {
  struct um_async_op *async_op = ptr;
  um_op_free(async_op->machine, async_op->op);
  free(ptr);
}

static const rb_data_type_t AsyncOp_type = {
    "UringMachine::AsyncOp",
    {AsyncOp_mark, AsyncOp_free, AsyncOp_size, AsyncOp_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE AsyncOp_allocate(VALUE klass) {
  struct um_async_op *async_op = malloc(sizeof(struct um_async_op));
  return TypedData_Wrap_Struct(klass, &AsyncOp_type, async_op);
}

inline struct um_async_op *AsyncOp_data(VALUE self) {
  return RTYPEDDATA_DATA(self);
}

VALUE AsyncOp_initialize(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  memset(async_op, 0, sizeof(struct um_async_op));
  async_op->self = self;
  return self;
}

void um_async_op_set(VALUE self, struct um *machine, struct um_op *op) {
  struct um_async_op *async_op = AsyncOp_data(self);
  async_op->machine = machine;
  async_op->op = op;
}

inline void raise_on_missing_op(struct um_async_op *async_op) {
  if (!async_op->op)
    rb_raise(rb_eRuntimeError, "Missing op");
}

inline int async_op_is_done(struct um_async_op *async_op) {
  return (async_op->op->flags & OP_F_COMPLETED);
}

VALUE AsyncOp_kind(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  switch(async_op->op->kind) {
    case OP_TIMEOUT:
      return SYM_timeout;
    default:
      rb_raise(rb_eRuntimeError, "Invalid op kind");
  }
}

VALUE AsyncOp_done_p(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  return async_op_is_done(async_op) ? Qtrue : Qfalse;
}

VALUE AsyncOp_result(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  return async_op_is_done(async_op) ? INT2NUM(async_op->op->result.res) : Qnil;
}

VALUE AsyncOp_cancelled_p(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  if (!async_op_is_done(async_op)) return Qnil;

  return (async_op->op->result.res == -ECANCELED) ? Qtrue : Qfalse;
}

VALUE AsyncOp_await(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  if (async_op_is_done(async_op))
    return INT2NUM(async_op->op->result.res);

  return um_async_op_await(async_op);
}

VALUE AsyncOp_cancel(VALUE self) {
  struct um_async_op *async_op = AsyncOp_data(self);
  raise_on_missing_op(async_op);

  if (!async_op_is_done(async_op))
    um_async_op_cancel(async_op);

  return self;
}

void Init_AsyncOp(void) {
  cAsyncOp = rb_define_class_under(cUM, "AsyncOp", rb_cObject);
  rb_define_alloc_func(cAsyncOp, AsyncOp_allocate);

  rb_define_method(cAsyncOp, "initialize", AsyncOp_initialize, 0);
  rb_define_method(cAsyncOp, "kind", AsyncOp_kind, 0);
  rb_define_method(cAsyncOp, "done?", AsyncOp_done_p, 0);
  rb_define_method(cAsyncOp, "result", AsyncOp_result, 0);
  rb_define_method(cAsyncOp, "cancelled?", AsyncOp_cancelled_p, 0);

  rb_define_method(cAsyncOp, "await", AsyncOp_await, 0);
  rb_define_method(cAsyncOp, "join", AsyncOp_await, 0);
  rb_define_method(cAsyncOp, "cancel", AsyncOp_cancel, 0);

  SYM_timeout = ID2SYM(rb_intern("timeout"));
}