#include "um.h"

VALUE cStream;
VALUE eStreamRESPError;

static void Stream_mark(void *ptr) {
  struct um_stream *stream = ptr;
  rb_gc_mark_movable(stream->self);
  rb_gc_mark_movable(stream->machine->self);
}

static void Stream_compact(void *ptr) {
  struct um_stream *stream = ptr;
  stream->self = rb_gc_location(stream->self);
}

static void Stream_free(void *ptr) {
  struct um_stream *stream = ptr;
  stream_teardown(stream);
}

static const rb_data_type_t Stream_type = {
  .wrap_struct_name = "UringMachine::Stream",
  .function = {
    .dmark = Stream_mark,
    .dfree = Stream_free,
    .dsize = NULL,
    .dcompact = Stream_compact
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_EMBEDDABLE
};

static VALUE Stream_allocate(VALUE klass) {
  struct um_stream *stream;
  VALUE self = TypedData_Make_Struct(klass, struct um_stream, &Stream_type, stream);
  return self;
}

static inline struct um_stream *um_get_stream(VALUE self) {
  struct um_stream *stream;
  TypedData_Get_Struct(self, struct um_stream, &Stream_type, stream);
  return stream;
}

VALUE Stream_initialize(VALUE self, VALUE machine, VALUE target) {
  struct um_stream *stream = um_get_stream(self);
  memset(stream, 0, sizeof(struct um_stream));

  RB_OBJ_WRITE(self, &stream->self, self);
  stream->machine = um_get_machine(machine);
  stream->mode = STREAM_BUFFER_POOL_READ;
  stream->fd = NUM2INT(target);

  return self;
}

VALUE Stream_get_line(VALUE self, VALUE limit) {
  struct um_stream *stream = um_get_stream(self);
  return stream_get_line(stream, Qnil, NUM2ULONG(limit));
}

VALUE Stream_get_string(VALUE self, VALUE len) {
  struct um_stream *stream = um_get_stream(self);
  return stream_get_string(stream, Qnil, NUM2LONG(len), 0, false);
}

VALUE Stream_resp_decode(VALUE self) {
  struct um_stream *stream = um_get_stream(self);
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

VALUE Stream_eof_p(VALUE self) {
  struct um_stream *stream = um_get_stream(self);
  return stream->eof ? Qtrue : Qfalse;
}

VALUE Stream_clear(VALUE self) {
  struct um_stream *stream = um_get_stream(self);
  stream_clear(stream);
  return self;
}

void Init_Stream(void) {
  cStream = rb_define_class_under(cUM, "Stream", rb_cObject);
  rb_define_alloc_func(cStream, Stream_allocate);

  rb_define_method(cStream, "initialize", Stream_initialize, 2);

  rb_define_method(cStream, "get_line", Stream_get_line, 1);
  rb_define_method(cStream, "get_string", Stream_get_string, 1);

  rb_define_method(cStream, "resp_decode", Stream_resp_decode, 0);
  rb_define_singleton_method(cStream, "resp_encode", Stream_resp_encode, 2);

  rb_define_method(cStream, "eof?", Stream_eof_p, 0);
  rb_define_method(cStream, "clear", Stream_clear, 0);

  eStreamRESPError = rb_define_class_under(cStream, "RESPError", rb_eStandardError);
}
