#include "um.h"
#include <stdlib.h>

VALUE cMutex;

static const rb_data_type_t Mutex_type = {
  .wrap_struct_name = "UringMachine::Mutex",
  .function = {
    .dmark = NULL,
    .dfree = RUBY_TYPED_DEFAULT_FREE,
    .dsize = NULL,
    .dcompact = NULL
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_EMBEDDABLE
};

static VALUE Mutex_allocate(VALUE klass) {
  struct um_mutex *mutex;
  return TypedData_Make_Struct(klass, struct um_mutex, &Mutex_type, mutex);
}

inline struct um_mutex *Mutex_data(VALUE self) {
  struct um_mutex *mutex;
  TypedData_Get_Struct(self, struct um_mutex, &Mutex_type, mutex);
  return mutex;
}

VALUE Mutex_initialize(VALUE self) {
  struct um_mutex *mutex = Mutex_data(self);
  um_mutex_init(mutex);
  return self;
}

void Init_Mutex(void) {
  cMutex = rb_define_class_under(cUM, "Mutex", rb_cObject);
  rb_define_alloc_func(cMutex, Mutex_allocate);

  rb_define_method(cMutex, "initialize", Mutex_initialize, 0);
}
