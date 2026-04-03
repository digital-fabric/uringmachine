#include <stdlib.h>
#include <ruby/io/buffer.h>
#include "um.h"

inline void io_add_segment(struct um_io *io, struct um_segment *segment) {
  segment->next = NULL;
  if (io->tail) {
    io->tail->next = segment;
    io->tail = segment;
  }
  else
    io->head = io->tail = segment;
  io->pending_bytes += segment->len;
}

inline int io_process_op_result(struct um_io *io, struct um_op_result *result) {
  if (likely(result->res > 0)) {
    if (likely(result->segment)) {
      io_add_segment(io, result->segment);
      result->segment = NULL;
    }
  }
  else
    io->eof = 1;

  return result->res;
}

#define IO_OP_FLAGS (OP_F_MULTISHOT | OP_F_BUFFER_POOL)

void io_multishot_op_start(struct um_io *io) {
  if (!io->op)
    io->op = um_op_acquire(io->machine);
  struct io_uring_sqe *sqe;

  bp_ensure_commit_level(io->machine);

  switch (io->mode) {
    case IO_FD:
      um_prep_op(io->machine, io->op, OP_READ_MULTISHOT, 2, IO_OP_FLAGS);
      sqe = um_get_sqe(io->machine, io->op);
      io_uring_prep_read_multishot(sqe, io->fd, 0, -1, BP_BGID);
      break;
    case IO_SOCKET:
      um_prep_op(io->machine, io->op, OP_RECV_MULTISHOT, 2, IO_OP_FLAGS);
      sqe = um_get_sqe(io->machine, io->op);
      io_uring_prep_recv_multishot(sqe, io->fd, NULL, 0, 0);
	    sqe->buf_group = BP_BGID;
	    sqe->flags |= IOSQE_BUFFER_SELECT;
      break;
    default:
      um_raise_internal_error("Invalid multishot op");
  }
  io->op->bp_commit_level = io->machine->bp_commit_level;
}

void io_multishot_op_stop(struct um_io *io) {
  assert(!io->op);

  if (!(io->op->flags & OP_F_CQE_DONE)) {
    io->op->flags |= OP_F_ASYNC;
    um_cancel_op(io->machine, io->op);
  }
  else
    um_op_release(io->machine, io->op);
  io->op = NULL;
}

void um_io_cleanup(struct um_io *io) {
  if (io->op) io_multishot_op_stop(io);

  while (io->head) {
    struct um_segment *next = io->head->next;
    um_segment_checkin(io->machine, io->head);
    io->head = next;
  }
  io->pending_bytes = 0;
}

// returns true if case of ENOBUFS error, sets more to true if more data forthcoming
inline int io_process_segments(
  struct um_io *io, size_t *total_bytes, int *more) {

  *more = 0;
  struct um_op_result *result = &io->op->result;
  io->op->flags &= ~OP_F_CQE_SEEN;
  while (result) {
    if (unlikely(result->res == -ENOBUFS)) {
      *more = 0;
      return true;
    }
    if (unlikely(result->res == -ECANCELED)) {
      *more = 0;
      return false;
    }
    um_raise_on_error_result(result->res);

    *more = (result->flags & IORING_CQE_F_MORE);
    *total_bytes += result->res;
    io_process_op_result(io, result);
    result = result->next;
  }
  return false;
}

void io_clear(struct um_io *io) {
  if (io->op && io->machine->ring_initialized) {
    if (OP_CQE_SEEN_P(io->op)) {
      size_t total_bytes = 0;
      int more = false;
      io_process_segments(io, &total_bytes, &more);
      um_op_multishot_results_clear(io->machine, io->op);
    }

    if (OP_CQE_DONE_P(io->op))
      um_op_release(io->machine, io->op);
    else
      um_cancel_op_and_discard_cqe(io->machine, io->op);

    io->op = NULL;
  }

  while (io->head) {
    struct um_segment *next = io->head->next;
    um_segment_checkin(io->machine, io->head);
    io->head = next;
  }
  io->pending_bytes = 0;

  if (io->working_buffer) {
    bp_buffer_checkin(io->machine, io->working_buffer);
    io->working_buffer = NULL;
  }
}

inline void io_await_segments(struct um_io *io) {
  if (unlikely(!io->op)) io_multishot_op_start(io);

  if (!OP_CQE_SEEN_P(io->op)) {
    io->op->flags &= ~OP_F_ASYNC;
    VALUE ret = um_yield(io->machine);
    io->op->flags |= OP_F_ASYNC;
    if (!OP_CQE_SEEN_P(io->op)) RAISE_IF_EXCEPTION(ret);
    RB_GC_GUARD(ret);
  }
}

int io_get_more_segments_bp(struct um_io *io) {
  size_t total_bytes = 0;
  int more = false;
  int enobufs = false;

  while (1) {
    if (unlikely(io->eof)) return 0;

    io_await_segments(io);
    enobufs = io_process_segments(io, &total_bytes, &more);
    um_op_multishot_results_clear(io->machine, io->op);
    if (unlikely(enobufs)) {
      int should_restart = io->pending_bytes < (io->machine->bp_buffer_size * 4);
      // int same_threshold = io->op->bp_commit_level == io->machine->bp_commit_level;

      // fprintf(stderr, "%p enobufs total: %ld pending: %ld threshold: %ld bc: %d (same: %d, restart: %d)\n",
      //   io,
      //   total_bytes, io->pending_bytes, io->machine->bp_commit_level,
      //   io->machine->bp_buffer_count,
      //   same_threshold, should_restart
      // );

      // If multiple IO ops are happening at the same time, they'll all
      // get ENOBUFS! We track the commit threshold in the op in order to
      // prevent running bp_handle_enobufs() more than once.

      if (should_restart) {
        if (io->op->bp_commit_level == io->machine->bp_commit_level)
          bp_handle_enobufs(io->machine);

        um_op_release(io->machine, io->op);
        io->op = NULL;
        // io_multishot_op_start(io);
      }
      else {
        um_op_release(io->machine, io->op);
        io->op = NULL;
      }

      if (total_bytes) return total_bytes;
    }
    else {
      if (more)
        io->op->flags &= ~OP_F_CQE_SEEN;
      if (total_bytes || io->eof) return total_bytes;
    }
  }
}

int io_get_more_segments_ssl(struct um_io *io) {
  if (!io->working_buffer)
    io->working_buffer = bp_buffer_checkout(io->machine);

  char *ptr = io->working_buffer->buf + io->working_buffer->pos;
  size_t maxlen = io->working_buffer->len - io->working_buffer->pos;
  int res = um_ssl_read_raw(io->machine, io->target, ptr, maxlen);
  if (res == 0) return 0;
  if (res < 0) rb_raise(eUMError, "Failed to read segment");

  struct um_segment *segment = bp_buffer_consume(io->machine, io->working_buffer, res);
  if ((size_t)res == maxlen) {
    bp_buffer_checkin(io->machine, io->working_buffer);
    io->working_buffer = NULL;
  }
  io_add_segment(io, segment);
  return 1;
}

int io_get_more_segments(struct um_io *io) {
  switch (io->mode) {
    case IO_FD:
    case IO_SOCKET:
      return io_get_more_segments_bp(io);
    case IO_SSL:
      return io_get_more_segments_ssl(io);
    default:
      rb_raise(eUMError, "Invalid IO mode");
  }
}

////////////////////////////////////////////////////////////////////////////////

inline void io_shift_head(struct um_io *io) {
  struct um_segment *consumed = io->head;
  io->head = consumed->next;
  if (!io->head) io->tail = NULL;
  um_segment_checkin(io->machine, consumed);
  io->pos = 0;
}

inline VALUE make_segment_io_buffer(struct um_segment *segment, size_t pos) {
  return rb_io_buffer_new(
    segment->ptr + pos, segment->len - pos,
    RB_IO_BUFFER_LOCKED|RB_IO_BUFFER_READONLY
  );
}

inline void io_skip(struct um_io *io, size_t inc, int safe_inc) {
  if (unlikely(io->eof && !io->head)) return;
  if (safe_inc && !io->tail && !io_get_more_segments(io)) return;

  while (inc) {
    size_t segment_len = io->head->len - io->pos;
    size_t inc_len = (segment_len <= inc) ? segment_len : inc;
    inc -= inc_len;
    io->pos += inc_len;
    io->consumed_bytes += inc_len;
    io->pending_bytes -= inc_len;
    if (io->pos == io->head->len) {
      io_shift_head(io);
      if (inc && safe_inc && !io->head) {
        if (!io_get_more_segments(io)) break;
      }
    }
  }
}

inline void io_read_each(struct um_io *io) {
  if (unlikely(io->eof && !io->head)) return;
  if (!io->tail && !io_get_more_segments(io)) return;

  struct um_segment *current = io->head;
  size_t pos = io->pos;

  VALUE buffer = Qnil;
  while (true) {
    struct um_segment *next = current->next;
    buffer = make_segment_io_buffer(current, pos);
    rb_yield(buffer);
    rb_io_buffer_free_locked(buffer);
    io_shift_head(io);

    if (!next) {
      if (!io_get_more_segments(io)) return;
    }
    current = io->head;
    pos = 0;
  }
  RB_GC_GUARD(buffer);
}

inline void io_copy(struct um_io *io, char *dest, size_t len) {
  while (len) {
    char *segment_ptr = io->head->ptr + io->pos;
    size_t segment_len = io->head->len - io->pos;
    size_t cpy_len = (segment_len <= len) ? segment_len : len;
    memcpy(dest, segment_ptr, cpy_len);

    len -= cpy_len;
    io->pos += cpy_len;
    io->consumed_bytes += cpy_len;
    io->pending_bytes -= cpy_len;
    dest += cpy_len;
    if (io->pos == io->head->len) io_shift_head(io);
  }
}

VALUE io_consume_string(struct um_io *io, VALUE out_buffer, size_t len, size_t inc, int safe_inc) {
  VALUE str = Qnil;
  if (!NIL_P(out_buffer)) {
    str = out_buffer;
    size_t str_len = RSTRING_LEN(str);
    if (str_len < len)
      rb_str_resize(str, len);
    else if (str_len > len)
      rb_str_set_len(str, len);
  }
  else
    str = rb_str_new(NULL, len);
  char *dest = RSTRING_PTR(str);

  io_copy(io, dest, len);
  io_skip(io, inc, safe_inc);
  return str;
  RB_GC_GUARD(str);
}

inline int trailing_cr_p(char *ptr, size_t len) {
  return ptr[len - 1] == '\r';
}

VALUE io_read_line(struct um_io *io, VALUE out_buffer, size_t maxlen) {
  if (unlikely(io->eof && !io->head)) return Qnil;
  if (!io->tail && !io_get_more_segments(io)) return Qnil;

  struct um_segment *last = NULL;
  struct um_segment *current = io->head;
  size_t remaining_len = maxlen;
  size_t total_len = 0;
  size_t inc = 1;
  size_t pos = io->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    size_t search_len = segment_len;
    if (maxlen && (search_len > remaining_len)) search_len = remaining_len;
    char *start = current->ptr + pos;
    char *lf_ptr = memchr(start, '\n', search_len);

    if (lf_ptr) {
      size_t len = lf_ptr - start;

      total_len += len;

      // search for \r
      if (total_len > 0) {
        if ((len &&          trailing_cr_p(start, len)) ||
            (!len && last && trailing_cr_p(last->ptr, last->len))
        ) {
          total_len -= 1;
          inc = 2;
        }
      }

      return io_consume_string(io, out_buffer, total_len, inc, false);
    }
    else {
      // not found, early return if segment len exceeds maxlen
      if (maxlen && segment_len >= maxlen) return Qnil;

      total_len += segment_len;
      remaining_len -= segment_len;
    }

    if (!current->next) {
      if (!io_get_more_segments(io)) {
        return Qnil;
      }
    }

    last = current;
    current = current->next;
    pos = 0;
  }
}

VALUE io_read(struct um_io *io, VALUE out_buffer, ssize_t len, size_t inc, int safe_inc) {
  if (unlikely(io->eof && !io->head)) return Qnil;
  if (!io->tail && !io_get_more_segments(io)) return Qnil;

  struct um_segment *current = io->head;
  size_t abs_len = labs(len);
  size_t remaining_len = abs_len;
  size_t total_len = 0;
  size_t pos = io->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    if (abs_len && segment_len > remaining_len) {
      segment_len = remaining_len;
    }
    total_len += segment_len;
    if (abs_len) {
      remaining_len -= segment_len;
      if (!remaining_len)
        return io_consume_string(io, out_buffer, total_len, inc, safe_inc);
    }

    if (!current->next) {
      if (len <= 0)
        return io_consume_string(io, out_buffer, total_len, inc, safe_inc);

      if (!io_get_more_segments(io))
        return Qnil;
    }
    current = current->next;
    pos = 0;
  }
}

static inline char delim_to_char(VALUE delim) {
  if (TYPE(delim) != T_STRING)
    rb_raise(rb_eArgError, "Delimiter must be a string");

  if (RSTRING_LEN(delim) != 1)
    rb_raise(eUMError, "Delimiter must be a single byte string");

  return *RSTRING_PTR(delim);
}

VALUE io_read_to_delim(struct um_io *io, VALUE out_buffer, VALUE delim, ssize_t maxlen) {
  char delim_char = delim_to_char(delim);

  if (unlikely(io->eof && !io->head)) return Qnil;
  if (unlikely(!io->tail) && !io_get_more_segments(io)) return Qnil;

  struct um_segment *current = io->head;
  size_t abs_maxlen = labs(maxlen);
  size_t remaining_len = abs_maxlen;
  size_t total_len = 0;
  size_t pos = io->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    size_t search_len = segment_len;
    if (maxlen && (search_len > remaining_len)) search_len = remaining_len;
    char *start = current->ptr + pos;
    char *delim_ptr = memchr(start, delim_char, search_len);

    if (delim_ptr) {
      size_t len = delim_ptr - start;
      total_len += len;
      return io_consume_string(io, out_buffer, total_len, 1, false);
    }
    else {
      // delimiter not found
      total_len += search_len;
      remaining_len -= search_len;

      if (abs_maxlen && total_len >= abs_maxlen)
        return (maxlen > 0) ? Qnil : io_consume_string(io, out_buffer, abs_maxlen, 1, false);
    }

    if (!current->next && !io_get_more_segments(io)) return Qnil;

    current = current->next;
    pos = 0;
  }
}

size_t io_write_raw(struct um_io *io, const char *buffer, size_t len) {
  switch (io->mode) {
    case IO_FD:
      return um_write_raw(io->machine, io->fd, buffer, len);
    case IO_SOCKET:
      return um_send_raw(io->machine, io->fd, buffer, len, 0);
    case IO_SSL:
      return um_ssl_write_raw(io->machine, io->target, buffer, len);
    default:
      rb_raise(eUMError, "Invalid IO mode");
  }
}

VALUE io_writev(struct um_io *io, int argc, VALUE *argv) {
  switch (io->mode) {
    case IO_FD:
      return um_writev(io->machine, io->fd, argc, argv);
    case IO_SOCKET:
      return um_sendv(io->machine, io->fd, argc, argv);
    case IO_SSL:
      return ULONG2NUM(um_ssl_writev(io->machine, io->target, argc, argv));
    default:
      rb_raise(eUMError, "Invalid IO mode");
  }
}

////////////////////////////////////////////////////////////////////////////////

VALUE resp_read_line(struct um_io *io, VALUE out_buffer) {
  if (unlikely(io->eof && !io->head)) return Qnil;
  if (!io->tail && !io_get_more_segments(io)) return Qnil;

  struct um_segment *current = io->head;
  size_t total_len = 0;
  size_t pos = io->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    char *start = current->ptr + pos;
    char *lf_ptr = memchr(start, '\r', segment_len);
    if (lf_ptr) {
      size_t len = lf_ptr - start;
      total_len += len;
      return io_consume_string(io, out_buffer, total_len, 2, true);
    }
    else
      total_len += segment_len;

    if (!current->next)
      if (!io_get_more_segments(io)) return Qnil;

    current = current->next;
  }
}

inline VALUE resp_read_string(struct um_io *io, ulong len, VALUE out_buffer) {
  return io_read(io, out_buffer, len, 2, true);
}

inline ulong resp_parse_length_field(const char *ptr, int len) {
  ulong acc = 0;
  for(int i = 1; i < len; i++)
    acc = acc * 10 + (ptr[i] - '0');
  return acc;
}

VALUE resp_decode_hash(struct um_io *io, VALUE out_buffer, ulong len) {
  VALUE hash = rb_hash_new();

  for (ulong i = 0; i < len; i++) {
    VALUE key = resp_read(io, out_buffer);
    VALUE value = resp_read(io, out_buffer);
    rb_hash_aset(hash, key, value);
    RB_GC_GUARD(key);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(hash);
  return hash;
}

VALUE resp_decode_array(struct um_io *io, VALUE out_buffer, ulong len) {
  VALUE array = rb_ary_new2(len);

  for (ulong i = 0; i < len; i++) {
    VALUE value = resp_read(io, out_buffer);
    rb_ary_push(array, value);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(array);
  return array;
}

static inline VALUE resp_decode_simple_string(char *ptr, ulong len) {
  return rb_str_new(ptr + 1, len - 1);
}

static inline VALUE resp_decode_string(struct um_io *io, VALUE out_buffer, ulong len) {
  return resp_read_string(io, len, out_buffer);
}

static inline VALUE resp_decode_string_with_encoding(struct um_io *io, VALUE out_buffer, ulong len) {
  VALUE with_enc = resp_read_string(io, len, out_buffer);
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
  VALUE err = rb_funcall(eIORESPError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

static inline VALUE resp_decode_error(struct um_io *io, VALUE out_buffer, ulong len) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  VALUE msg = resp_decode_string(io, out_buffer, len);
  VALUE err = rb_funcall(eIORESPError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

VALUE resp_read(struct um_io *io, VALUE out_buffer) {
  VALUE msg = resp_read_line(io, out_buffer);
  if (msg == Qnil) return Qnil;

  char *ptr = RSTRING_PTR(msg);
  ulong len = RSTRING_LEN(msg);
  ulong data_len;
  if (len == 0) return Qnil;

  switch (ptr[0]) {
    case '%': // hash
    case '|': // attributes hash
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_hash(io, out_buffer, data_len);

    case '*': // array
    case '~': // set
    case '>': // pub/sub push
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_array(io, out_buffer, data_len);

    case '+': // simple string
      return resp_decode_simple_string(ptr, len);
    case '$': // string
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string(io, out_buffer, data_len);
    case '=': // string with encoding
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string_with_encoding(io, out_buffer, data_len);

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
      return resp_decode_error(io, out_buffer, data_len);
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
