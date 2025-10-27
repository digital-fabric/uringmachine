#include "um.h"

VALUE cStream;

static void Stream_mark(void *ptr) {
  struct um_stream *stream = ptr;
  rb_gc_mark_movable(stream->buffer);
}

static void Stream_compact(void *ptr) {
  struct um_stream *stream = ptr;
  stream->buffer = rb_gc_location(stream->buffer);
}

static const rb_data_type_t Stream_type = {
  .wrap_struct_name = "UringMachine::Stream",
  .function = {
    .dmark = Stream_mark,
    .dfree = RUBY_TYPED_DEFAULT_FREE,
    .dsize = NULL,
    .dcompact = Stream_compact
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_EMBEDDABLE
};

static VALUE Stream_allocate(VALUE klass) {
  struct um_stream *stream;
  return TypedData_Make_Struct(klass, struct um_stream, &Stream_type, stream);
}

static inline struct um_stream *Stream_data(VALUE self) {
  struct um_stream *stream;
  TypedData_Get_Struct(self, struct um_stream, &Stream_type, stream);
  return stream;
}

VALUE Stream_initialize(VALUE self, VALUE machine, VALUE fd) {
  struct um_stream *stream = Stream_data(self);

  stream->machine = um_get_machine(machine);
  stream->fd = NUM2ULONG(fd);
  stream->buffer = rb_utf8_str_new_literal("");
  rb_str_resize(stream->buffer, 1 << 16); // 64KB
  rb_str_set_len(stream->buffer, 0);

  stream->len = 0;
  stream->pos = 0;
  stream->eof = 0;

  return self;
}

VALUE Stream_get_line(VALUE self, VALUE buf, VALUE limit) {
  struct um_stream *stream = Stream_data(self);
  if (unlikely(stream->eof)) return Qnil;

  return stream_get_line(stream, buf, NUM2LONG(limit));
}

VALUE Stream_get_string(VALUE self, VALUE buf, VALUE len) {
  struct um_stream *stream = Stream_data(self);
  if (unlikely(stream->eof)) return Qnil;

  return stream_get_string(stream, buf, NUM2LONG(len));
}

VALUE Stream_resp_decode(VALUE self) {
  struct um_stream *stream = Stream_data(self);
  if (unlikely(stream->eof)) return Qnil;

  VALUE out_buffer = rb_utf8_str_new_literal("");
  VALUE obj = resp_decode(stream, out_buffer);
  RB_GC_GUARD(out_buffer);
  return obj;
}

VALUE Stream_resp_encode(VALUE self, VALUE str, VALUE obj) {
  struct um_write_buffer buf;
  write_buffer_init(&buf, str);
  rb_str_modify(str);
  resp_encode(&buf, obj);
  write_buffer_update_len(&buf);
  return str;
}

void Init_Stream(void) {
  VALUE cStream = rb_define_class_under(cUM, "Stream", rb_cObject);
  rb_define_alloc_func(cStream, Stream_allocate);

  rb_define_method(cStream, "initialize", Stream_initialize, 2);

  rb_define_method(cStream, "get_line", Stream_get_line, 2);
  rb_define_method(cStream, "get_string", Stream_get_string, 2);

  rb_define_method(cStream, "resp_decode", Stream_resp_decode, 0);
  rb_define_singleton_method(cStream, "resp_encode", Stream_resp_encode, 2);
}
