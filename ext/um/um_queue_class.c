#include "um.h"
#include <stdlib.h>

VALUE cQueue;

static void Queue_mark(void *ptr) {
  struct um_queue *queue = ptr;
  um_queue_mark(queue);
}

static void Queue_compact(void *ptr) {
  struct um_queue *queue = ptr;
  um_queue_compact(queue);
}

static void Queue_free(void *ptr) {
  struct um_queue *queue = ptr;
  um_queue_free(queue);
}

static size_t Queue_size(const void *ptr) {
  return sizeof(struct um_queue);
}

static const rb_data_type_t Queue_type = {
    "UringMachineQueue",
    {Queue_mark, Queue_free, Queue_size, Queue_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE Queue_allocate(VALUE klass) {
  struct um_queue *queue = malloc(sizeof(struct um_queue));
  return TypedData_Wrap_Struct(klass, &Queue_type, queue);
}

inline struct um_queue *Queue_data(VALUE self) {
  return RTYPEDDATA_DATA(self);
}

VALUE Queue_initialize(VALUE self) {
  struct um_queue *queue = Queue_data(self);
  RB_OBJ_WRITE(self, &queue->self, self);
  um_queue_init(queue);
  return self;
}

VALUE Queue_count(VALUE self) {
  struct um_queue *queue = Queue_data(self);
  return UINT2NUM(queue->count);
}

void Init_Queue(void) {
  cQueue = rb_define_class_under(cUM, "Queue", rb_cObject);
  rb_define_alloc_func(cQueue, Queue_allocate);

  rb_define_method(cQueue, "initialize", Queue_initialize, 0);
  rb_define_method(cQueue, "count", Queue_count, 0);
}
