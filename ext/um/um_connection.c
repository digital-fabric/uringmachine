#include <stdlib.h>
#include <ruby/io/buffer.h>
#include "um.h"

inline void connection_add_segment(struct um_connection *conn, struct um_segment *segment) {
  segment->next = NULL;
  if (conn->tail) {
    conn->tail->next = segment;
    conn->tail = segment;
  }
  else
    conn->head = conn->tail = segment;
  conn->pending_bytes += segment->len;
}

inline int connection_process_op_result(struct um_connection *conn, struct um_op_result *result) {
  if (likely(result->res > 0)) {
    if (likely(result->segment)) {
      connection_add_segment(conn, result->segment);
      result->segment = NULL;
    }
  }
  else
    conn->eof = 1;

  return result->res;
}

#define CONNECTION_OP_FLAGS (OP_F_MULTISHOT | OP_F_BUFFER_POOL)

void connection_multishot_op_start(struct um_connection *conn) {
  if (!conn->op)
    conn->op = um_op_acquire(conn->machine);
  struct io_uring_sqe *sqe;

  bp_ensure_commit_level(conn->machine);

  switch (conn->mode) {
    case CONNECTION_FD:
      um_prep_op(conn->machine, conn->op, OP_READ_MULTISHOT, 2, CONNECTION_OP_FLAGS);
      sqe = um_get_sqe(conn->machine, conn->op);
      io_uring_prep_read_multishot(sqe, conn->fd, 0, -1, BP_BGID);
      break;
    case CONNECTION_SOCKET:
      um_prep_op(conn->machine, conn->op, OP_RECV_MULTISHOT, 2, CONNECTION_OP_FLAGS);
      sqe = um_get_sqe(conn->machine, conn->op);
      io_uring_prep_recv_multishot(sqe, conn->fd, NULL, 0, 0);
	    sqe->buf_group = BP_BGID;
	    sqe->flags |= IOSQE_BUFFER_SELECT;
      break;
    default:
      um_raise_internal_error("Invalid multishot op");
  }
  conn->op->bp_commit_level = conn->machine->bp_commit_level;
}

void connection_multishot_op_stop(struct um_connection *conn) {
  assert(!conn->op);

  if (!(conn->op->flags & OP_F_CQE_DONE)) {
    conn->op->flags |= OP_F_ASYNC;
    um_cancel_op(conn->machine, conn->op);
  }
  else
    um_op_release(conn->machine, conn->op);
  conn->op = NULL;
}

void um_connection_cleanup(struct um_connection *conn) {
  if (conn->op) connection_multishot_op_stop(conn);

  while (conn->head) {
    struct um_segment *next = conn->head->next;
    um_segment_checkin(conn->machine, conn->head);
    conn->head = next;
  }
  conn->pending_bytes = 0;
}

// returns true if case of ENOBUFS error, sets more to true if more data forthcoming
inline int connection_process_segments(
  struct um_connection *conn, size_t *total_bytes, int *more) {

  *more = 0;
  struct um_op_result *result = &conn->op->result;
  conn->op->flags &= ~OP_F_CQE_SEEN;
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
    connection_process_op_result(conn, result);
    result = result->next;
  }
  return false;
}

void connection_clear(struct um_connection *conn) {
  if (conn->op && conn->machine->ring_initialized) {
    if (OP_CQE_SEEN_P(conn->op)) {
      size_t total_bytes = 0;
      int more = false;
      connection_process_segments(conn, &total_bytes, &more);
      um_op_multishot_results_clear(conn->machine, conn->op);
    }

    if (OP_CQE_DONE_P(conn->op))
      um_op_release(conn->machine, conn->op);
    else
      um_cancel_op_and_discard_cqe(conn->machine, conn->op);

    conn->op = NULL;
  }

  while (conn->head) {
    struct um_segment *next = conn->head->next;
    um_segment_checkin(conn->machine, conn->head);
    conn->head = next;
  }
  conn->pending_bytes = 0;

  if (conn->working_buffer) {
    bp_buffer_checkin(conn->machine, conn->working_buffer);
    conn->working_buffer = NULL;
  }
}

inline void connection_await_segments(struct um_connection *conn) {
  if (unlikely(!conn->op)) connection_multishot_op_start(conn);

  if (!OP_CQE_SEEN_P(conn->op)) {
    conn->op->flags &= ~OP_F_ASYNC;
    VALUE ret = um_yield(conn->machine);
    conn->op->flags |= OP_F_ASYNC;
    if (!OP_CQE_SEEN_P(conn->op)) RAISE_IF_EXCEPTION(ret);
    RB_GC_GUARD(ret);
  }
}

int connection_get_more_segments_bp(struct um_connection *conn) {
  size_t total_bytes = 0;
  int more = false;
  int enobufs = false;

  while (1) {
    if (unlikely(conn->eof)) return 0;

    connection_await_segments(conn);
    enobufs = connection_process_segments(conn, &total_bytes, &more);
    um_op_multishot_results_clear(conn->machine, conn->op);
    if (unlikely(enobufs)) {
      int should_restart = conn->pending_bytes < (conn->machine->bp_buffer_size * 4);
      // int same_threshold = conn->op->bp_commit_level == conn->machine->bp_commit_level;

      // fprintf(stderr, "%p enobufs total: %ld pending: %ld threshold: %ld bc: %d (same: %d, restart: %d)\n",
      //   conn,
      //   total_bytes, conn->pending_bytes, conn->machine->bp_commit_level,
      //   conn->machine->bp_buffer_count,
      //   same_threshold, should_restart
      // );

      // If multiple connection ops are happening at the same time, they'll all
      // get ENOBUFS! We track the commit threshold in the op in order to
      // prevent running bp_handle_enobufs() more than once.

      if (should_restart) {
        if (conn->op->bp_commit_level == conn->machine->bp_commit_level)
          bp_handle_enobufs(conn->machine);

        um_op_release(conn->machine, conn->op);
        conn->op = NULL;
        // connection_multishot_op_start(conn);
      }
      else {
        um_op_release(conn->machine, conn->op);
        conn->op = NULL;
      }

      if (total_bytes) return total_bytes;
    }
    else {
      if (more)
        conn->op->flags &= ~OP_F_CQE_SEEN;
      if (total_bytes || conn->eof) return total_bytes;
    }
  }
}

int connection_get_more_segments_ssl(struct um_connection *conn) {
  if (!conn->working_buffer)
    conn->working_buffer = bp_buffer_checkout(conn->machine);

  char *ptr = conn->working_buffer->buf + conn->working_buffer->pos;
  size_t maxlen = conn->working_buffer->len - conn->working_buffer->pos;
  int res = um_ssl_read_raw(conn->machine, conn->target, ptr, maxlen);
  if (res == 0) return 0;
  if (res < 0) rb_raise(eUMError, "Failed to read segment");

  struct um_segment *segment = bp_buffer_consume(conn->machine, conn->working_buffer, res);
  if ((size_t)res == maxlen) {
    bp_buffer_checkin(conn->machine, conn->working_buffer);
    conn->working_buffer = NULL;
  }
  connection_add_segment(conn, segment);
  return 1;
}

int connection_get_more_segments(struct um_connection *conn) {
  switch (conn->mode) {
    case CONNECTION_FD:
    case CONNECTION_SOCKET:
      return connection_get_more_segments_bp(conn);
    case CONNECTION_SSL:
      return connection_get_more_segments_ssl(conn);
    default:
      rb_raise(eUMError, "Invalid connection mode");
  }
}

////////////////////////////////////////////////////////////////////////////////

inline void connection_shift_head(struct um_connection *conn) {
  struct um_segment *consumed = conn->head;
  conn->head = consumed->next;
  if (!conn->head) conn->tail = NULL;
  um_segment_checkin(conn->machine, consumed);
  conn->pos = 0;
}

inline VALUE make_segment_io_buffer(struct um_segment *segment, size_t pos) {
  return rb_io_buffer_new(
    segment->ptr + pos, segment->len - pos,
    RB_IO_BUFFER_LOCKED|RB_IO_BUFFER_READONLY
  );
}

inline void connection_skip(struct um_connection *conn, size_t inc, int safe_inc) {
  if (unlikely(conn->eof && !conn->head)) return;
  if (safe_inc && !conn->tail && !connection_get_more_segments(conn)) return;

  while (inc) {
    size_t segment_len = conn->head->len - conn->pos;
    size_t inc_len = (segment_len <= inc) ? segment_len : inc;
    inc -= inc_len;
    conn->pos += inc_len;
    conn->consumed_bytes += inc_len;
    conn->pending_bytes -= inc_len;
    if (conn->pos == conn->head->len) {
      connection_shift_head(conn);
      if (inc && safe_inc && !conn->head) {
        if (!connection_get_more_segments(conn)) break;
      }
    }
  }
}

inline void connection_read_each(struct um_connection *conn) {
  if (unlikely(conn->eof && !conn->head)) return;
  if (!conn->tail && !connection_get_more_segments(conn)) return;

  struct um_segment *current = conn->head;
  size_t pos = conn->pos;

  VALUE buffer = Qnil;
  while (true) {
    struct um_segment *next = current->next;
    buffer = make_segment_io_buffer(current, pos);
    rb_yield(buffer);
    rb_io_buffer_free_locked(buffer);
    connection_shift_head(conn);

    if (!next) {
      if (!connection_get_more_segments(conn)) return;
    }
    current = conn->head;
    pos = 0;
  }
  RB_GC_GUARD(buffer);
}

inline void connection_copy(struct um_connection *conn, char *dest, size_t len) {
  while (len) {
    char *segment_ptr = conn->head->ptr + conn->pos;
    size_t segment_len = conn->head->len - conn->pos;
    size_t cpy_len = (segment_len <= len) ? segment_len : len;
    memcpy(dest, segment_ptr, cpy_len);

    len -= cpy_len;
    conn->pos += cpy_len;
    conn->consumed_bytes += cpy_len;
    conn->pending_bytes -= cpy_len;
    dest += cpy_len;
    if (conn->pos == conn->head->len) connection_shift_head(conn);
  }
}

VALUE connection_consume_string(struct um_connection *conn, VALUE out_buffer, size_t len, size_t inc, int safe_inc) {
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

  connection_copy(conn, dest, len);
  connection_skip(conn, inc, safe_inc);
  return str;
  RB_GC_GUARD(str);
}

inline int trailing_cr_p(char *ptr, size_t len) {
  return ptr[len - 1] == '\r';
}

VALUE connection_read_line(struct um_connection *conn, VALUE out_buffer, size_t maxlen) {
  if (unlikely(conn->eof && !conn->head)) return Qnil;
  if (!conn->tail && !connection_get_more_segments(conn)) return Qnil;

  struct um_segment *last = NULL;
  struct um_segment *current = conn->head;
  size_t remaining_len = maxlen;
  size_t total_len = 0;
  size_t inc = 1;
  size_t pos = conn->pos;

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

      return connection_consume_string(conn, out_buffer, total_len, inc, false);
    }
    else {
      // not found, early return if segment len exceeds maxlen
      if (maxlen && segment_len >= maxlen) return Qnil;

      total_len += segment_len;
      remaining_len -= segment_len;
    }

    if (!current->next) {
      if (!connection_get_more_segments(conn)) {
        return Qnil;
      }
    }

    last = current;
    current = current->next;
    pos = 0;
  }
}

VALUE connection_read(struct um_connection *conn, VALUE out_buffer, ssize_t len, size_t inc, int safe_inc) {
  if (unlikely(conn->eof && !conn->head)) return Qnil;
  if (!conn->tail && !connection_get_more_segments(conn)) return Qnil;

  struct um_segment *current = conn->head;
  size_t abs_len = labs(len);
  size_t remaining_len = abs_len;
  size_t total_len = 0;
  size_t pos = conn->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    if (abs_len && segment_len > remaining_len) {
      segment_len = remaining_len;
    }
    total_len += segment_len;
    if (abs_len) {
      remaining_len -= segment_len;
      if (!remaining_len)
        return connection_consume_string(conn, out_buffer, total_len, inc, safe_inc);
    }

    if (!current->next) {
      if (len <= 0)
        return connection_consume_string(conn, out_buffer, total_len, inc, safe_inc);

      if (!connection_get_more_segments(conn))
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

VALUE connection_read_to_delim(struct um_connection *conn, VALUE out_buffer, VALUE delim, ssize_t maxlen) {
  char delim_char = delim_to_char(delim);

  if (unlikely(conn->eof && !conn->head)) return Qnil;
  if (unlikely(!conn->tail) && !connection_get_more_segments(conn)) return Qnil;

  struct um_segment *current = conn->head;
  size_t abs_maxlen = labs(maxlen);
  size_t remaining_len = abs_maxlen;
  size_t total_len = 0;
  size_t pos = conn->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    size_t search_len = segment_len;
    if (maxlen && (search_len > remaining_len)) search_len = remaining_len;
    char *start = current->ptr + pos;
    char *delim_ptr = memchr(start, delim_char, search_len);

    if (delim_ptr) {
      size_t len = delim_ptr - start;
      total_len += len;
      return connection_consume_string(conn, out_buffer, total_len, 1, false);
    }
    else {
      // delimiter not found
      total_len += search_len;
      remaining_len -= search_len;

      if (abs_maxlen && total_len >= abs_maxlen)
        return (maxlen > 0) ? Qnil : connection_consume_string(conn, out_buffer, abs_maxlen, 1, false);
    }

    if (!current->next && !connection_get_more_segments(conn)) return Qnil;

    current = current->next;
    pos = 0;
  }
}

VALUE connection_writev(struct um_connection *conn, int argc, VALUE *argv) {
  switch (conn->mode) {
    case CONNECTION_FD:
      return um_writev(conn->machine, conn->fd, argc, argv);
    case CONNECTION_SOCKET:
      return um_sendv(conn->machine, conn->fd, argc, argv);
    case CONNECTION_SSL:
      return ULONG2NUM(um_ssl_writev(conn->machine, conn->target, argc, argv));
    default:
      rb_raise(eUMError, "Invalid connection mode");
  }
}

////////////////////////////////////////////////////////////////////////////////

VALUE resp_read_line(struct um_connection *conn, VALUE out_buffer) {
  if (unlikely(conn->eof && !conn->head)) return Qnil;
  if (!conn->tail && !connection_get_more_segments(conn)) return Qnil;

  struct um_segment *current = conn->head;
  size_t total_len = 0;
  size_t pos = conn->pos;

  while (true) {
    size_t segment_len = current->len - pos;
    char *start = current->ptr + pos;
    char *lf_ptr = memchr(start, '\r', segment_len);
    if (lf_ptr) {
      size_t len = lf_ptr - start;
      total_len += len;
      return connection_consume_string(conn, out_buffer, total_len, 2, true);
    }
    else
      total_len += segment_len;

    if (!current->next)
      if (!connection_get_more_segments(conn)) return Qnil;

    current = current->next;
  }
}

inline VALUE resp_read_string(struct um_connection *conn, ulong len, VALUE out_buffer) {
  return connection_read(conn, out_buffer, len, 2, true);
}

inline ulong resp_parse_length_field(const char *ptr, int len) {
  ulong acc = 0;
  for(int i = 1; i < len; i++)
    acc = acc * 10 + (ptr[i] - '0');
  return acc;
}

VALUE resp_decode_hash(struct um_connection *conn, VALUE out_buffer, ulong len) {
  VALUE hash = rb_hash_new();

  for (ulong i = 0; i < len; i++) {
    VALUE key = resp_read(conn, out_buffer);
    VALUE value = resp_read(conn, out_buffer);
    rb_hash_aset(hash, key, value);
    RB_GC_GUARD(key);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(hash);
  return hash;
}

VALUE resp_decode_array(struct um_connection *conn, VALUE out_buffer, ulong len) {
  VALUE array = rb_ary_new2(len);

  for (ulong i = 0; i < len; i++) {
    VALUE value = resp_read(conn, out_buffer);
    rb_ary_push(array, value);
    RB_GC_GUARD(value);
  }

  RB_GC_GUARD(array);
  return array;
}

static inline VALUE resp_decode_simple_string(char *ptr, ulong len) {
  return rb_str_new(ptr + 1, len - 1);
}

static inline VALUE resp_decode_string(struct um_connection *conn, VALUE out_buffer, ulong len) {
  return resp_read_string(conn, len, out_buffer);
}

static inline VALUE resp_decode_string_with_encoding(struct um_connection *conn, VALUE out_buffer, ulong len) {
  VALUE with_enc = resp_read_string(conn, len, out_buffer);
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
  VALUE err = rb_funcall(eConnectionRESPError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

static inline VALUE resp_decode_error(struct um_connection *conn, VALUE out_buffer, ulong len) {
  static ID ID_new = 0;
  if (!ID_new) ID_new = rb_intern("new");

  VALUE msg = resp_decode_string(conn, out_buffer, len);
  VALUE err = rb_funcall(eConnectionRESPError, ID_new, 1, msg);
  RB_GC_GUARD(msg);
  return err;
}

VALUE resp_read(struct um_connection *conn, VALUE out_buffer) {
  VALUE msg = resp_read_line(conn, out_buffer);
  if (msg == Qnil) return Qnil;

  char *ptr = RSTRING_PTR(msg);
  ulong len = RSTRING_LEN(msg);
  ulong data_len;
  if (len == 0) return Qnil;

  switch (ptr[0]) {
    case '%': // hash
    case '|': // attributes hash
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_hash(conn, out_buffer, data_len);

    case '*': // array
    case '~': // set
    case '>': // pub/sub push
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_array(conn, out_buffer, data_len);

    case '+': // simple string
      return resp_decode_simple_string(ptr, len);
    case '$': // string
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string(conn, out_buffer, data_len);
    case '=': // string with encoding
      data_len = resp_parse_length_field(ptr, len);
      return resp_decode_string_with_encoding(conn, out_buffer, data_len);

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
      return resp_decode_error(conn, out_buffer, data_len);
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
