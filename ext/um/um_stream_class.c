#include "um.h"
#include <arpa/inet.h>

VALUE cStream;

static void Stream_mark(void *ptr) {
  struct um_stream *stream = ptr;
  rb_gc_mark_movable(stream->self);
}

static void Stream_compact(void *ptr) {
  struct um_stream *stream = ptr;
  stream->self = rb_gc_location(stream->self);
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

  stream->machine = RTYPEDDATA_DATA(machine);
  stream->fd = NUM2ULONG(fd);
  stream->buffer = rb_utf8_str_new_literal("");
  rb_str_resize(stream->buffer, 1 << 16); // 64KB

  stream->len = 0;
  stream->pos = 0;
  stream->eof = 0;

  return self;
}

static inline void Stream_check_truncate_buffer(struct um_stream *stream) {
  if ((stream->pos == stream->len) && (stream->len >= 1 << 12)) {
    rb_str_set_len(stream->buffer, 0);
    stream->len = stream->pos = 0;
  }
  if (stream->pos >= 1 << 12) {
    char *base = RSTRING_PTR(stream->buffer);
    int len_rest = stream->len - stream->pos;
    memmove(base, base + stream->pos, len_rest);
    rb_str_set_len(stream->buffer, len_rest);
    stream->len = len_rest;
    stream->pos = 0;
  }
}

static inline int Stream_read_more(struct um_stream *stream) {
  Stream_check_truncate_buffer(stream);

  int ofs = (stream->pos == 0) ? 0 : -1;
  VALUE ret = um_read(stream->machine, stream->fd, stream->buffer, 1 << 12, ofs);

  uint read_count = NUM2ULONG(ret);

  if (read_count == 0) return 0;

  stream->len += read_count;
  return 1;
}

VALUE Stream_get_line(VALUE self) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  while (true) {
    char * lf_ptr = memchr(start, '\n', stream->len - stream->pos);
    if (lf_ptr) {
      ulong len = lf_ptr - start;
      if (len && (start[len - 1] == '\r')) len -= 1;

      VALUE str = rb_str_new(start, len);
      stream->pos += lf_ptr - start + 1;
      return str;
    }

    if (!Stream_read_more(stream)) return Qnil;
  }
}

VALUE Stream_get_string(VALUE self, VALUE len) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  ulong ulen = NUM2ULONG(len);

  while (stream->len - stream->pos < ulen)
    if (!Stream_read_more(stream)) return Qnil;
  
  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  VALUE str = rb_str_new(start, ulen);
  stream->pos += ulen;
  return str;
}

VALUE Stream_resp_get_line(VALUE self) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  while (true) {
    char * lf_ptr = memchr(start, '\r', stream->len - stream->pos);
    if (lf_ptr) {
      ulong len = lf_ptr - start;

      VALUE str = rb_str_new(start, len);
      stream->pos += lf_ptr - start + 2;
      return str;
    }

    if (!Stream_read_more(stream)) return Qnil;
  }
}

VALUE Stream_resp_get_string(VALUE self, VALUE len) {
  struct um_stream *stream = RTYPEDDATA_DATA(self);
  if (unlikely(stream->eof)) return Qnil;

  ulong ulen = NUM2ULONG(len);
  ulong read_len = ulen + 2;

  while (stream->len - stream->pos < read_len)
    if (!Stream_read_more(stream)) return Qnil;
  
  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  VALUE str = rb_str_new(start, ulen);
  stream->pos += read_len;
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
}
