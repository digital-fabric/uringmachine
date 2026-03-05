#include "um.h"
#include <stdlib.h>

void stream_teardown(struct um_stream *stream) {
  stream_clear(stream);
}

// void stream_setup(VALUE self, struct um_stream *stream, struct um *machine, int fd, enum um_op_kind kind) {
//   memset(stream, 0, sizeof(struct um_stream));
//   stream->self = self;

//   RB_OBJ_WRITE(self, &stream->self, self);

//   stream->machine = machine;
//   stream->fd = fd;
//   stream->kind = kind;
// }

// static inline struct um_stream *um_stream_prepare(struct um *machine, int fd, enum um_op_kind kind) {
//   static ID ID_allocate;
//   if (!ID_allocate) ID_allocate = rb_intern_const("allocate");
//   VALUE stream_self = rb_funcall(cStream, ID_allocate, 0);
//   struct um_stream *stream = um_get_stream(stream_self);
//   stream_setup(stream_self, stream, machine, fd, kind);
//   return stream;
// }

inline void stream_add_segment(struct um_stream *stream, struct um_segment *segment) {
  segment->next = NULL;
  if (stream->tail) {
    stream->tail->next = segment;
    stream->tail = segment;
  }
  else {
    stream->head = stream->tail = segment;
  }
}

inline int stream_multishot_op_process_result(struct um_stream *stream, struct um_op_result *result) { 
  if (likely(result->res > 0)) {
    if (likely(result->segment)) {
      stream_add_segment(stream, result->segment);
      result->segment = NULL;
    }
  }
  else {
    stream->eof = 1;
  }

  return result->res;
}

#define STREAM_OP_FLAGS (OP_F_MULTISHOT | OP_F_BUFFER_POOL)

void stream_multishot_op_start(struct um_stream *stream) {
  struct um_buffer_group *group = um_buffer_group_select(stream->machine);
  stream->op = um_op_acquire(stream->machine);
  struct io_uring_sqe *sqe;

  switch (stream->mode) {
    case STREAM_BUFFER_POOL_READ:
      um_prep_op(stream->machine, stream->op, OP_READ_MULTISHOT, 2, STREAM_OP_FLAGS);
      stream->op->buffer_group = group;
      sqe = um_get_sqe(stream->machine, stream->op);
      io_uring_prep_read_multishot(sqe, stream->fd, 0, -1, group->bgid);
      break;
    case STREAM_BUFFER_POOL_RECV:
      um_prep_op(stream->machine, stream->op, OP_RECV_MULTISHOT, 2, STREAM_OP_FLAGS);
      stream->op->buffer_group = group;
      sqe = um_get_sqe(stream->machine, stream->op);
      io_uring_prep_recv_multishot(sqe, stream->fd, NULL, 0, 0);
	    sqe->buf_group = group->bgid;
	    sqe->flags |= IOSQE_BUFFER_SELECT;
      break;
    default:
      um_raise_internal_error("Invalid multishot op");
  }
}

void stream_multishot_op_stop(struct um_stream *stream) {
  // if (!stream->op) return;
  // if (!(stream->op->flags & OP_F_CQE_DONE)) {
  //   stream->op->flags |= OP_F_FREE_ON_COMPLETE;
  //   // printf("\n* cancelling... op: %p\n", stream->op);
  //   um_cancel_op(stream->machine, stream->op);
  // }
  // else
  //   um_op_free(stream->machine, stream->op);
  // stream->op = NULL;
}

void um_stream_cleanup(struct um_stream *stream) {
  if (stream->op)
    stream_multishot_op_stop(stream);

  while (stream->head) {
    struct um_segment *next = stream->head->next;
    um_segment_checkin(stream->machine, stream->head);
    stream->head = next;
  }
}

// returns true if case of ENOBUFS
inline int stream_process_segments(
  struct um_stream *stream, size_t *total_bytes, int *more) {

  int count = 0;
  *more = 0;  
  struct um_op_result *result = &stream->op->result;
  while (result) {
    count++;
    uint dbg_bid = result->flags >> IORING_CQE_BUFFER_SHIFT;
    uint dbg_more = !!(result->flags & IORING_CQE_F_MORE);
    uint dbg_buf_more = !!(result->flags & IORING_CQE_F_BUF_MORE);
    fprintf(stderr, "* process_segment result: %p next: %p res: %d flags: %06x bgid: %d bid: %d more: %d buf_more: %d\n",
      result, result->next, result->res, result->flags, stream->op->buffer_group->bgid, dbg_bid, dbg_more, dbg_buf_more
    );
    if (result->res < 0) exit(1);
    if (result->res == -ENOBUFS) {
      *more = 0;
      return true;
    }
    if (result->res == -ECANCELED) {
      *more = 0;
      return false;
    }
    // if (result->res < 0) printf("got err: %d\n", result->res);
    um_raise_on_error_result(result->res);

    *more = (result->flags & IORING_CQE_F_MORE);
    *total_bytes += result->res;
    stream_multishot_op_process_result(stream, result);
    result = result->next;
  }
  return false;
}

void stream_clear(struct um_stream *stream) {
  if (stream->op) {
    if (OP_CQE_SEEN_P(stream->op)) {
      size_t total_bytes = 0;
      int more = false;
      stream_process_segments(stream, &total_bytes, &more);
      um_op_multishot_results_clear(stream->machine, stream->op);
    }
    if (!OP_CQE_DONE_P(stream->op)) {
      um_cancel_op_and_discard_cqe(stream->machine, stream->op);
    }
  }

  while (stream->head) {
    struct um_segment *next = stream->head->next;
    um_segment_checkin(stream->machine, stream->head);
    stream->head = next;
  }
}

inline void stream_await_segments(struct um_stream *stream) {
  if (!stream->op) stream_multishot_op_start(stream);

  if (!OP_CQE_SEEN_P(stream->op)) {
    stream->op->flags &= ~OP_F_ASYNC;
    VALUE ret = um_yield(stream->machine);
    stream->op->flags |= OP_F_ASYNC;
    if (!OP_CQE_SEEN_P(stream->op)) RAISE_IF_EXCEPTION(ret);
    RB_GC_GUARD(ret);
  }
}

int stream_get_more_segments(struct um_stream *stream) {
  size_t total_bytes = 0;
  int more = false;
  int enobufs = false;

  while (1) {
    if (stream->eof) return 0;

    stream_await_segments(stream);
    enobufs = stream_process_segments(stream, &total_bytes, &more);
    um_op_multishot_results_clear(stream->machine, stream->op);
    if (enobufs) {
      stream_multishot_op_stop(stream);

      if (total_bytes) return total_bytes;
      // else {
      //   printf("* ENOBUFS! commited: %d\n", stream->machine->metrics.buffer_pool_commited);
      //   printf("  available: %d\n", avail);        
      //   for (int i = 0; i++; i < 1000000) {
      //     um_schedule(stream->machine, rb_fiber_current(), Qnil);
      //     VALUE ret = um_switch(stream->machine);
      //     RAISE_IF_EXCEPTION(ret);
      //   }
      // }
      // return total_bytes;
    }
    else {
      // printf("done waiting: total_bytes: %ld more: %d eof: %d\n",
      //   total_bytes, more, stream->eof
      // );
      if (more) {
        stream->op->flags &= ~OP_F_CQE_SEEN;
        // printf("* reset F_COMPLETED op: %p total: %ld eof: %d\n", stream->op, total_bytes, stream->eof);
      }
      else {
        stream_multishot_op_stop(stream);
      }
      if (total_bytes || stream->eof) return total_bytes;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////


VALUE stream_consume_string(struct um_stream *stream, VALUE out_buffer, size_t len, size_t inc, int safe_inc) {
  VALUE str = Qnil;
  if (!NIL_P(out_buffer)) {
    str = out_buffer;
    if ((size_t)RSTRING_LEN(str) < len)
      rb_str_resize(str, len);
  }
  else
    str = rb_str_new(NULL, len);
  char *str_ptr = RSTRING_PTR(str);
  while (len) {
    char *segment_ptr = stream->head->ptr + stream->pos;
    size_t segment_len = stream->head->len - stream->pos;
    size_t cpy_len = (segment_len <= len) ? segment_len : len;

    // struct um_buffer *buffer = stream->head->buffer;
    // size_t sz = sizeof(struct um_buffer);
    // char *buffer_ptr = buffer->buf;
    // fprintf(stderr, "  buffer: %p cpy_len: %ld segment_len: %ld len: %ld pos: %ld\n", buffer, cpy_len, segment_len, len, stream->pos);
    // size_t ofs = segment_ptr - buffer_ptr;
    // fprintf(stderr, "  segment_ptr: %p buffer_ptr: %p ofs: %ld buffer_pos: %ld sz: %ld\n", segment_ptr, buffer_ptr, ofs, buffer->pos, sz);
    // fprintf(stderr, "  \"%*s\"\n", (int)cpy_len, segment_ptr);

    // VALUE tmp = rb_str_new(segment_ptr, cpy_len);
    // static int tmp_i = 0;
    // INSPECT("  ", tmp);
    // tmp_i++;
    // if (tmp_i >= 10) exit(1);
    // printf("* consume cur %p next: %p pos: %ld len: %ld seg_len: %ld cpy_len: %ld\n",
    //   stream->head, stream->head->next, stream->pos, len, segment_len, cpy_len
    // );
    // if (cpy_len > 1) cpy_len = 1;
    memcpy(str_ptr, segment_ptr, cpy_len);
    // memset(str_ptr, 0, cpy_len);

    len -= cpy_len;
    stream->pos += cpy_len;
    str_ptr += cpy_len;
    if (stream->pos == stream->head->len) {
      struct um_segment *consumed = stream->head;
      stream->head = consumed->next;
      if (!stream->head) stream->tail = NULL;
      um_segment_checkin(stream->machine, consumed);
      stream->pos = 0;
    }
  }

  while (inc) {
    size_t segment_len = stream->head->len - stream->pos;
    size_t inc_len = (segment_len <= inc) ? segment_len : inc;
    inc -= inc_len;
    stream->pos += inc_len;
    if (stream->pos == stream->head->len) {
      struct um_segment *consumed = stream->head;
      stream->head = consumed->next;
      um_segment_checkin(stream->machine, consumed);
      if (!stream->head) {
        stream->tail = NULL;
        if (inc && safe_inc) {
          if (!stream_get_more_segments(stream)) break;
        }
      }
      stream->pos = 0;
    }
  }
  // printf("  str_len: %ld\n", RSTRING_LEN(str));
  return str;
  RB_GC_GUARD(str);
}

// inline void stream_advance(struct um_stream *stream, size_t inc) {
//   while (inc) {
//     size_t segment_len = stream->head->len - stream->pos;
//     size_t inc_len = (segment_len <= inc) ? segment_len : inc;
//     inc -= inc_len;
//     stream->pos += inc_len;
//     if (stream->pos == stream->head->len) {
//       struct um_segment *consumed = stream->head;
//       stream->head = consumed->next;
//       um_segment_checkin(stream->machine, consumed);
//       if (!stream->head) {
//         stream->tail = NULL;
//         if (!stream_get_more_segments(stream)) return;
//       }
//       stream->pos = 0;
//     }
//   }
// }

VALUE stream_get_line(struct um_stream *stream, VALUE out_buffer, size_t maxlen) {
  // int x = stream->eof && !stream->head;
  // printf("get_line eof: %d head: %p x: %d\n", stream->eof, stream->head, x);
  if (unlikely(stream->eof && !stream->head)) return Qnil;

  if (unlikely(!stream->tail))
    if (!stream_get_more_segments(stream)) {
      // printf("* return Qnil 1\n");
      return Qnil;
    }

  struct um_segment *last = NULL;
  struct um_segment *current = stream->head;
  size_t remaining_len = maxlen;
  size_t total_len = 0;
  size_t inc = 1;
  size_t pos = stream->pos;
  
  while (true) {
    size_t segment_len = current->len - pos;
    // printf("* get_line current: %p next: %p pos: %ld seg_len: %ld\n",
      // current, current->next, stream->pos, segment_len);
    size_t search_len = segment_len;
    if (maxlen && (search_len > remaining_len)) search_len = remaining_len;
    char *start = current->ptr + pos;
    char *lf_ptr = memchr(start, '\n', search_len);

    if (lf_ptr) {
      size_t len = lf_ptr - start;
      // printf("  len: %ld\n", len);

      total_len += len;

      // search for \r
      if (total_len) {
        if (len) {
          if  (start[len - 1] == '\r') {
            total_len -= 1;
            inc = 2;
          }
        }
        else {
          if (last && (((char *)last->ptr)[last->len - 1] == '\r')) {
            total_len -= 1;
            inc = 2;
          }
        }
      }

      return stream_consume_string(stream, out_buffer, total_len, inc, false);
    }
    else {
      if (maxlen && segment_len >= maxlen) return Qnil;

      total_len += segment_len;
      remaining_len -= segment_len;
    }

    if (!current->next) {
      if (!stream_get_more_segments(stream)) {
        // printf("* return Qnil 2\n");
        return Qnil;
      }
    }

    last = current;
    current = current->next;
    pos = 0;
  }
}

VALUE stream_get_string(struct um_stream *stream, VALUE out_buffer, ssize_t len, size_t inc, int safe_inc) {
  if (unlikely(stream->eof && !stream->head)) return Qnil;
  if (unlikely(!stream->tail)) {
    if (!stream_get_more_segments(stream)) return Qnil;
  }

  struct um_segment *current = stream->head;
  size_t abs_len = labs(len);
  size_t remaining_len = abs_len;
  size_t total_len = 0;
  size_t pos = stream->pos;

  while (true) {
    // printf("* current bgid: %d bid: %d pos: %ld len: %ld next: %p\n", current->bgid, current->bid, pos, current->len, current->next);
    size_t segment_len = current->len - pos;
    if (abs_len && segment_len > remaining_len) {
      segment_len = remaining_len;
    }
    total_len += segment_len;
    // printf("  segment_len: %ld total_len: %ld abs_len: %ld remaining_len: %ld\n",
    //   segment_len, total_len, abs_len, remaining_len
    // );
    if (abs_len) {
      remaining_len -= segment_len;
      if (!remaining_len) {
        // printf("  consume1 len: %ld\n", total_len);
        return stream_consume_string(stream, out_buffer, total_len, inc, safe_inc);
      }
    }

    if (!current->next) {
      if (len <= 0) {
        // printf("  consume2 len: %ld\n", total_len);
        return stream_consume_string(stream, out_buffer, total_len, inc, safe_inc);
      }

      if (!stream_get_more_segments(stream)) {
        // printf("* Qnil 3! eof: %d\n", stream->eof);
        return Qnil;
      }
    }
    current = current->next;
    pos = 0;
  }
}

// char resp_get_char(struct um_stream *stream) {
//   if (unlikely(stream->eof && !stream->head)) return Qnil;

//   if (unlikely(!stream->tail))
//     if (!stream_get_more_segments(stream)) return Qnil;

//   struct um_segment *current = stream->head;
//   size_t total_len = 0;

//   while (true) {
//     size_t segment_len = current->len - stream->pos;
//     if (segment_len >= 1) {
//       char c = ((char *)current->ptr)[stream->pos];
//       stream->pos++;
//       if (stream->pos == current->len) {
//         stream->head = stream->head->next;
//         um_segment_checkin(stream->machine, current);
//         stream->pos = 0;
//       }
//     }
//   }
// }

VALUE resp_get_line(struct um_stream *stream, VALUE out_buffer) {
  if (unlikely(stream->eof && !stream->head)) return Qnil;

  if (unlikely(!stream->tail))
    if (!stream_get_more_segments(stream)) return Qnil;

  struct um_segment *current = stream->head;
  size_t total_len = 0;
  size_t pos = stream->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    char *start = current->ptr + pos;
    char *lf_ptr = memchr(start, '\r', segment_len);
    if (lf_ptr) {
      size_t len = lf_ptr - start;
      total_len += len;
      return stream_consume_string(stream, out_buffer, total_len, 2, true);
    }
    else
      total_len += segment_len;

    if (!current->next)
      if (!stream_get_more_segments(stream)) return Qnil;

    current = current->next;
    pos = 0;
  }
}

inline VALUE resp_get_string(struct um_stream *stream, ulong len, VALUE out_buffer) {
  return stream_get_string(stream, out_buffer, len, 2, true);
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
  VALUE err = rb_funcall(eStreamRESPError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

static inline VALUE resp_decode_error(struct um_stream *stream, VALUE out_buffer, ulong len) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  VALUE msg = resp_decode_string(stream, out_buffer, len);
  VALUE err = rb_funcall(eStreamRESPError, ID_new, 1, msg);
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
      um_raise_internal_error("Big integers are not supported");
    case ',': // float
      return resp_decode_float(ptr);

    case '-': // simple error
      return resp_decode_simple_error(ptr, len);
    case '!': // error
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_error(stream, out_buffer, data_len);
    default:
      um_raise_internal_error("Invalid character encountered");
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
      um_raise_internal_error("Can't encode object");
  }
}
