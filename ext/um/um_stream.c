#include "um.h"

static inline void stream_check_truncate_buffer(struct um_stream *stream) {
  if ((stream->pos == stream->len) && (stream->len >= 1 << 12)) {
    rb_str_modify(stream->buffer);
    rb_str_set_len(stream->buffer, 0);
    stream->len = 0;
    stream->pos = 0;
  }
  else if (stream->pos >= 1 << 12) {
    rb_str_modify(stream->buffer);
    char *base = RSTRING_PTR(stream->buffer);
    int len_rest = stream->len - stream->pos;
    memmove(base, base + stream->pos, len_rest);
    rb_str_set_len(stream->buffer, len_rest);
    stream->len = len_rest;
    stream->pos = 0;
  }
}

int stream_read_more(struct um_stream *stream) {
  stream_check_truncate_buffer(stream);

  size_t maxlen = 1 << 12;
  size_t capa = rb_str_capacity(stream->buffer);
  if (capa - stream->pos < maxlen)
    rb_str_modify_expand(stream->buffer, maxlen - (capa - stream->pos));
  
  rb_str_modify(stream->buffer);
  char *ptr = RSTRING_PTR(stream->buffer) + stream->pos;
  size_t ret = um_read_raw(stream->machine, stream->fd, ptr, maxlen);

  if (ret == 0) {
    stream->eof = 1;
    return 0;
  }

  stream->len = stream->pos + ret;
  rb_str_set_len(stream->buffer, stream->len);
  return 1;
}

// ensure string can hold at least len bytes
static inline void str_expand(VALUE str, size_t len) {
  size_t capa = rb_str_capacity(str);
  if (capa < len + 1) rb_str_modify_expand(str, len + 1 - capa);
}

VALUE resp_get_line(struct um_stream *stream, VALUE out_buffer) {
  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  while (true) {
    char * lf_ptr = memchr(start, '\r', stream->len - stream->pos);
    if (lf_ptr) {
      ulong len = lf_ptr - start;
      stream->pos += len + 2;

      if (NIL_P(out_buffer)) {
        VALUE str = rb_str_new(start, len + 1);
        rb_str_set_len(str, len);
        RSTRING_PTR(str)[len] = 0;
        return str;
      }

      str_expand(out_buffer, len + 1);
      char *dest_ptr = RSTRING_PTR(out_buffer);
      memcpy(dest_ptr, start, len);
      dest_ptr[len] = 0; // add null at end
      rb_str_set_len(out_buffer, len);
      return out_buffer;
    }

    if (stream_read_more(stream))
      // buffer ptr and pos may have changed after reading
      start = RSTRING_PTR(stream->buffer) + stream->pos;
    else
      return Qnil;
  }
}

VALUE resp_get_string(struct um_stream *stream, ulong len, VALUE out_buffer) {
  ulong read_len = len + 2;

  while (stream->len - stream->pos < read_len)
    if (!stream_read_more(stream)) return Qnil;
  
  char *start = RSTRING_PTR(stream->buffer) + stream->pos;
  stream->pos += read_len;

  if (NIL_P(out_buffer)) return rb_utf8_str_new(start, len);

  str_expand(out_buffer, len + 1);
  char *dest_ptr = RSTRING_PTR(out_buffer);
  memcpy(dest_ptr, start, len);
  dest_ptr[len] = 0; // add null at end
  rb_str_set_len(out_buffer, len);
  return out_buffer;
}

inline ulong resp_parse_length_field(const char *ptr, int len) {
  return strtoul(ptr + 1, NULL, 10);
}

VALUE resp_decode_hash(struct um_stream *stream, VALUE out_buffer, ulong len) {
  VALUE hash = rb_hash_new();

  for (ulong i = 0; i < len; i++) {
    VALUE key = resp_decode(stream, out_buffer);
    VALUE value = resp_decode(stream, out_buffer);
    rb_hash_aset(hash, key, value);
    RB_GC_GUARD(key);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(hash);
  return hash;
}

VALUE resp_decode_array(struct um_stream *stream, VALUE out_buffer, ulong len) {
  VALUE array = rb_ary_new2(len);

  for (ulong i = 0; i < len; i++) {
    VALUE value = resp_decode(stream, out_buffer);
    rb_ary_push(array, value);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(array);
  return array;
}

static inline VALUE resp_decode_simple_string(char *ptr, ulong len) {
  return rb_str_new(ptr + 1, len - 1);
}

static inline VALUE resp_decode_string(struct um_stream *stream, VALUE out_buffer, ulong len) {
  return resp_get_string(stream, len, out_buffer);
}

static inline VALUE resp_decode_string_with_encoding(struct um_stream *stream, VALUE out_buffer, ulong len) {
  VALUE with_enc = resp_get_string(stream, len, out_buffer);
  char *ptr = RSTRING_PTR(with_enc);
  len = RSTRING_LEN(with_enc);
  if ((len < 4) || (ptr[3] != ':')) return Qnil;

  return rb_utf8_str_new(ptr + 4, len - 4);
}

static inline VALUE resp_decode_integer(char *ptr) {
  long value = strtol(ptr + 1, NULL, 10);
  return LONG2NUM(value);    
}

static inline VALUE resp_decode_float(char *ptr) {
  double value = strtod(ptr + 1, NULL);
  return DBL2NUM(value);
}

static inline VALUE resp_decode_simple_error(char *ptr, ulong len) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  VALUE msg = rb_str_new(ptr + 1, len - 1);
  VALUE err = rb_funcall(rb_eRuntimeError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

static inline VALUE resp_decode_error(struct um_stream *stream, VALUE out_buffer, ulong len) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  VALUE msg = resp_decode_string(stream, out_buffer, len);
  VALUE err = rb_funcall(rb_eRuntimeError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

VALUE resp_decode(struct um_stream *stream, VALUE out_buffer) {
  VALUE msg = resp_get_line(stream, out_buffer);
  if (msg == Qnil) return Qnil;

  char *ptr = RSTRING_PTR(msg);
  ulong len = RSTRING_LEN(msg);
  ulong data_len;
  if (len == 0) return Qnil;
  
  switch (ptr[0]) {
    case '%': // hash
    case '|': // attributes hash
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_hash(stream, out_buffer, data_len);
    
    case '*': // array
    case '~': // set
    case '>': // pub/sub push
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_array(stream, out_buffer, data_len);

    case '+': // simple string
      return resp_decode_simple_string(ptr, len);
    case '$': // string
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string(stream, out_buffer, data_len);
    case '=': // string with encoding
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string_with_encoding(stream, out_buffer, data_len);

    case '_': // null
      return Qnil;
    case '#': // boolean
      return (len > 1) && (ptr[1] == 't') ? Qtrue : Qfalse;

    case ':': // integer
      return resp_decode_integer(ptr);
    case '(': // big integer
      rb_raise(rb_eRuntimeError, "Big integers are not supported");
    case ',': // float
      return resp_decode_float(ptr);

    case '-': // simple error
      return resp_decode_simple_error(ptr, len);
    case '!': // error
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_error(stream, out_buffer, data_len);
    default:
      rb_raise(rb_eRuntimeError, "Invalid character encountered");
  }
  
  RB_GC_GUARD(msg);
}

void write_buffer_init(struct um_write_buffer *buf, VALUE str) {
  size_t capa = 1 << 12;
  size_t len = RSTRING_LEN(str);
  while (capa < len) capa += 1 << 12;
  
  rb_str_resize(str, capa);
  rb_str_set_len(str, len);
  buf->str = str;
  buf->capa = capa;
  buf->len = len;
  buf->ptr = RSTRING_PTR(str);
}

static inline void write_buffer_expand(struct um_write_buffer *buf, size_t newsize) {
  if (buf->capa < newsize) {
    size_t old_capa = buf->capa;
    while (buf->capa < newsize) buf->capa += 1 << 12;
    rb_str_modify_expand(buf->str, buf->capa - old_capa);
    buf->ptr = RSTRING_PTR(buf->str);
  }
}

static inline void write_buffer_append(struct um_write_buffer *buf, const char *ptr, size_t len) {
  size_t total_len = buf->len + len;
  write_buffer_expand(buf, total_len);

  memcpy(buf->ptr + buf->len, ptr, len);
  buf->len = total_len;
}

static inline void write_buffer_append_cstr(struct um_write_buffer *buf, const char *str) {
  write_buffer_append(buf, str, strlen(str));
}

static inline void write_buffer_append_resp_bulk_string(struct um_write_buffer *buf, VALUE str) {
  // leave enough place for prefix and postfix
  size_t str_len = RSTRING_LEN(str);
  size_t total_len = buf->len + str_len + 16;
  write_buffer_expand(buf, total_len);


  int prefix_len = sprintf(buf->ptr + buf->len, "$%ld\r\n", str_len);
  const char *src = RSTRING_PTR(str);
  memcpy(buf->ptr + buf->len + prefix_len, src, str_len);
  buf->ptr[buf->len + prefix_len + str_len + 0] = '\r';
  buf->ptr[buf->len + prefix_len + str_len + 1] = '\n';
  buf->len += prefix_len + str_len + 2;
}

inline void write_buffer_update_len(struct um_write_buffer *buf) {
  rb_str_set_len(buf->str, buf->len);
}

struct resp_encode_hash_ctx {
  struct um_write_buffer *buf;
  VALUE obj;
};

int resp_encode_hash_entry(VALUE key, VALUE value, VALUE arg) {
  struct resp_encode_hash_ctx *ctx = (struct resp_encode_hash_ctx *)arg;

  resp_encode(ctx->buf, key);
  resp_encode(ctx->buf, value);
  return 0;
}

void resp_encode(struct um_write_buffer *buf, VALUE obj) {
  char tmp[60];

  switch (TYPE(obj)) {
    case T_NIL:
      return write_buffer_append_cstr(buf, "_\r\n");
      return;
    case T_FALSE:
      write_buffer_append_cstr(buf, "#f\r\n");
      return;
    case T_TRUE:
      write_buffer_append_cstr(buf, "#t\r\n");
      return;
    case T_FIXNUM:
      sprintf(tmp, ":%ld\r\n", NUM2LONG(obj));
      write_buffer_append_cstr(buf, tmp);
      return;
    case T_FLOAT:
      sprintf(tmp, ",%lg\r\n", NUM2DBL(obj));
      write_buffer_append_cstr(buf, tmp);
      return;
    case T_STRING:
      write_buffer_append_resp_bulk_string(buf, obj);
      return;
    case T_ARRAY:
      {
        ulong len = RARRAY_LEN(obj);
        sprintf(tmp, "*%ld\r\n", len);
        write_buffer_append_cstr(buf, tmp);
        for (ulong i = 0; i < len; i++)
          resp_encode(buf, rb_ary_entry(obj, i));
        return;
      }
    case T_HASH:
      {
        ulong len = rb_hash_size_num(obj);
        sprintf(tmp, "%%%ld\r\n", len);
        write_buffer_append_cstr(buf, tmp);

        struct resp_encode_hash_ctx ctx = { buf, obj };
        rb_hash_foreach(obj, resp_encode_hash_entry, (VALUE)&ctx);
        return;
      }
    default:
      rb_raise(rb_eRuntimeError, "Can't encode object");
  }
}
