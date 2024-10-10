#include "um.h"
#include <stdlib.h>

VALUE cMutex;

static void Mutex_mark(void *ptr) {
  struct um_futex *futex = ptr;
  rb_gc_mark_movable(futex->self);
}

static void Mutex_compact(void *ptr) {
  struct um_futex *futex = ptr;
  futex->self = rb_gc_location(futex->self);
}

static size_t Mutex_size(const void *ptr) {
  return sizeof(struct um_futex);
}

static const rb_data_type_t Mutex_type = {
    "UringMachineMutex",
    {Mutex_mark, free, Mutex_size, Mutex_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE Mutex_allocate(VALUE klass) {
  struct um_futex *futex = malloc(sizeof(struct um_futex));
  return TypedData_Wrap_Struct(klass, &Mutex_type, futex);
}

inline struct um_futex *Mutex_data(VALUE self) {
  return RTYPEDDATA_DATA(self);
}

void um_mutex_init(uint32_t *futex);

VALUE Mutex_initialize(VALUE self) {
  struct um_futex *futex = Mutex_data(self);
  um_mutex_init(&futex->value);
  return self;
}

void Init_Mutex(void) {
  cMutex = rb_define_class_under(cUM, "Mutex", rb_cObject);
  rb_define_alloc_func(cMutex, Mutex_allocate);

  rb_define_method(cMutex, "initialize", Mutex_initialize, 0);
}
