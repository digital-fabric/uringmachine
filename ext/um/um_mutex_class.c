#include "um.h"
#include <stdlib.h>

VALUE cMutex;

static void Mutex_mark(void *ptr) {
  struct um_mutex *mutex = ptr;
  rb_gc_mark_movable(mutex->self);
}

static void Mutex_compact(void *ptr) {
  struct um_mutex *mutex = ptr;
  mutex->self = rb_gc_location(mutex->self);
}

static size_t Mutex_size(const void *ptr) {
  return sizeof(struct um_mutex);
}

static const rb_data_type_t Mutex_type = {
    "UringMachineMutex",
    {Mutex_mark, free, Mutex_size, Mutex_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE Mutex_allocate(VALUE klass) {
  struct um_mutex *mutex = malloc(sizeof(struct um_mutex));
  return TypedData_Wrap_Struct(klass, &Mutex_type, mutex);
}

inline struct um_mutex *Mutex_data(VALUE self) {
  return RTYPEDDATA_DATA(self);
}

VALUE Mutex_initialize(VALUE self) {
  struct um_mutex *mutex = Mutex_data(self);
  mutex->self = self;
  um_mutex_init(mutex);
  return self;
}

void Init_Mutex(void) {
  cMutex = rb_define_class_under(cUM, "Mutex", rb_cObject);
  rb_define_alloc_func(cMutex, Mutex_allocate);

  rb_define_method(cMutex, "initialize", Mutex_initialize, 0);
}
