#include "um.h"

VALUE cStream;

static void Stream_mark(void *ptr) {
  struct um_stream *stream = ptr;
  rb_gc_mark_movable(stream->self);
  rb_gc_mark_movable(stream->buffer);
}

static void Stream_compact(void *ptr) {
  struct um_stream *stream = ptr;
  stream->self = rb_gc_location(stream->self);
  stream->buffer = rb_gc_location(stream->buffer);
}

static void Stream_free(void *ptr) {
  free(ptr);
}

static size_t Stream_size(const void *ptr) {
  return sizeof(struct um_stream);
}

static const rb_data_type_t Stream_type = {
    "UringMachine::Stream",
    {Stream_mark, Stream_free, Stream_size, Stream_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE Stream_allocate(VALUE klass) {
  struct um_stream *stream = ALLOC(struct um_stream);

  return TypedData_Wrap_Struct(klass, &Stream_type, stream);
}

VALUE Stream_initialize(VALUE self, VALUE machine, VALUE fd) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);

  stream->self = self;
  
  stream->machine = RTYPEDDATA_DATA(machine);
  stream->fd = NUM2ULONG(fd);
  stream->buffer = rb_utf8_str_new_literal("");
  rb_str_resize(stream->buffer, 1 << 16); // 64KB
  rb_str_set_len(stream->buffer, 0);

  stream->len = 0;
  stream->pos = 0;
  stream->eof = 0;

  return self;
}

VALUE Stream_get_line(VALUE self) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  return stream_get_line(stream);
}

VALUE Stream_get_string(VALUE self, VALUE len) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  return stream_get_string(stream, NUM2ULONG(len));
}

VALUE Stream_resp_get_line(VALUE self) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  VALUE line = resp_get_line(stream, Qnil);
  RB_GC_GUARD(line);
  return line;
}

VALUE Stream_resp_get_string(VALUE self, VALUE len) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  VALUE str = resp_get_string(stream, NUM2ULONG(len), Qnil);
  RB_GC_GUARD(str);
  return str;
}

VALUE Stream_resp_decode(VALUE self) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
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
  
  rb_define_method(cStream, "get_line", Stream_get_line, 0);
  rb_define_method(cStream, "get_string", Stream_get_string, 1);

  rb_define_method(cStream, "resp_get_line", Stream_resp_get_line, 0);
  rb_define_method(cStream, "resp_get_string", Stream_resp_get_string, 1);

  rb_define_method(cStream, "resp_decode", Stream_resp_decode, 0);
  
  rb_define_singleton_method(cStream, "resp_encode", Stream_resp_encode, 2);
}
