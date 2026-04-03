#include "um.h"

VALUE cIO;
VALUE eIORESPError;

VALUE SYM_fd;
VALUE SYM_socket;
VALUE SYM_ssl;

inline int io_has_target_obj_p(struct um_io *conn) {
  switch (conn->mode) {
    case IO_SSL:
    case IO_STRING:
    case IO_IO_BUFFER:
      return true;
    default:
      return false;
  }
}

inline void io_mark_segments(struct um_io *conn) {
  struct um_segment *curr = conn->head;
  while (curr) {
    // rb_gc_mark_movable(curr->obj);
    curr = curr->next;
  }
}

inline void io_compact_segments(struct um_io *conn) {
  struct um_segment *curr = conn->head;
  while (curr) {
    // curr->obj = rb_gc_location(curr->obj);
    curr = curr->next;
  }
}

static void IO_mark(void *ptr) {
  struct um_io *conn = ptr;
  rb_gc_mark_movable(conn->self);
  rb_gc_mark_movable(conn->machine->self);

  if (io_has_target_obj_p(conn)) {
    rb_gc_mark_movable(conn->target);
    io_mark_segments(conn);
  }
}

static void IO_compact(void *ptr) {
  struct um_io *conn = ptr;
  conn->self = rb_gc_location(conn->self);

  if (io_has_target_obj_p(conn)) {
    conn->target = rb_gc_location(conn->target);
    io_compact_segments(conn);
  }
}

static void IO_free(void *ptr) {
  struct um_io *conn = ptr;
  io_clear(conn);
}

static const rb_data_type_t IO_type = {
  .wrap_struct_name = "UringMachine::IO",
  .function = {
    .dmark = IO_mark,
    .dfree = IO_free,
    .dsize = NULL,
    .dcompact = IO_compact
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_EMBEDDABLE
};

static VALUE IO_allocate(VALUE klass) {
  struct um_io *conn;
  VALUE self = TypedData_Make_Struct(klass, struct um_io, &IO_type, conn);
  return self;
}

static inline struct um_io *um_get_io(VALUE self) {
  struct um_io *conn;
  TypedData_Get_Struct(self, struct um_io, &IO_type, conn);
  return conn;
}

static inline void io_set_target(struct um_io *conn, VALUE target, enum um_io_mode mode) {
  conn->mode = mode;
  switch (mode) {
    case IO_FD:
    case IO_SOCKET:
      conn->fd = NUM2INT(target);
      return;
    case IO_SSL:
      conn->target = target;
      um_ssl_set_bio(conn->machine, target);
      return;
    default:
      rb_raise(eUMError, "Invalid connection mode");
  }
}

static inline void io_setup(struct um_io *conn, VALUE target, VALUE mode) {
  conn->working_buffer = NULL;
  if (NIL_P(mode)) {
    if (TYPE(target) == T_DATA)
      io_set_target(conn, target, IO_SSL);
    else
      io_set_target(conn, target, IO_FD);
  }
  else if (mode == SYM_fd)
    io_set_target(conn, target, IO_FD);
  else if (mode == SYM_socket)
    io_set_target(conn, target, IO_SOCKET);
  else if (mode == SYM_ssl)
    io_set_target(conn, target, IO_SSL);
  else
    rb_raise(eUMError, "Invalid connection mode");
}

/* call-seq:
 *   UM::Stream.new(machine, fd, mode = nil) -> conn
 *   machine.connection(fd, mode = nil) -> conn
 *   machine.connection(fd, mode = nil) { |conn| ... }
 *
 * Initializes a new connection with the given UringMachine instance, target and
 * optional mode. The target maybe a file descriptor, or an instance of
 * OpenSSL::SSL::SSLSocket. In case of an SSL socket, the mode should be :ssl.
 *
 * @param machine [UringMachine] UringMachine instance
 * @param target [integer, OpenSSL::SSL::SSLSocket] connection target: file descriptor or SSL socket
 * @param mode [Symbol] optional connection mode: :fd, :socket, :ssl
 * @return [void]
 */
VALUE IO_initialize(int argc, VALUE *argv, VALUE self) {
  VALUE machine;
  VALUE target;
  VALUE mode;
  rb_scan_args(argc, argv, "21", &machine, &target, &mode);

  struct um_io *conn = um_get_io(self);
  memset(conn, 0, sizeof(struct um_io));

  RB_OBJ_WRITE(self, &conn->self, self);
  conn->machine = um_get_machine(machine);
  io_setup(conn, target, mode);

  return self;
}

/* call-seq:
 *   conn.mode -> mode
 *
 * Returns the connection mode.
 *
 * @return [Symbol] connection mode
 */
VALUE IO_mode(VALUE self) {
  struct um_io *conn = um_get_io(self);
  switch (conn->mode) {
    case IO_FD:  return SYM_fd;
    case IO_SOCKET:  return SYM_socket;
    case IO_SSL:      return SYM_ssl;
    default:              return Qnil;
  }
  return Qnil;
}

/* call-seq:
 *   conn.read_line(limit) -> str
 *
 * Reads from the string until a newline character is encountered. Returns the
 * line without the newline delimiter. If limit is 0, the line length is not
 * limited. If no newline delimiter is found before EOF, returns nil.
 *
 * @param limit [integer] maximum line length (0 means no limit)
 * @return [String, nil] read data or nil
 */
VALUE IO_read_line(VALUE self, VALUE limit) {
  struct um_io *conn = um_get_io(self);
  return io_read_line(conn, Qnil, NUM2ULONG(limit));
}

/* call-seq:
 *   conn.read(len) -> str
 *
 * Reads len bytes from the conn. If len is 0, reads all available bytes. If
 * len is negative, reads up to -len available bytes. If len is positive and eof
 * is encountered before len bytes are read, returns nil.
 *
 * @param len [integer] number of bytes to read
 * @return [String, nil] read data or nil
 */
VALUE IO_read(VALUE self, VALUE len) {
  struct um_io *conn = um_get_io(self);
  return io_read(conn, Qnil, NUM2LONG(len), 0, false);
}

/* call-seq:
 *   conn.read_to_delim(delim, limit) -> str
 *
 * Reads from the string until a the given delimiter is encountered. Returns the
 * line without the delimiter. If limit is 0, the length is not limited. If a
 * delimiter is not found before EOF and limit is 0 or greater, returns nil.
 *
 * If no delimiter is found before EOF and limit is negative, returns the
 * buffered data up to EOF or until the absolute-value length limit is reached.
 *
 * The `delim` parameter must be a single byte string.
 *
 * @param delim [String] delimiter (single byte) @param limit [integer] maximum
 * line length (0 means no limit) @return [String, nil] read data or nil
 */
VALUE IO_read_to_delim(VALUE self, VALUE delim, VALUE limit) {
  struct um_io *conn = um_get_io(self);
  return io_read_to_delim(conn, Qnil, delim, NUM2LONG(limit));
}

/* call-seq:
 *   conn.skip(len) -> len
 *
 * Skips len bytes in the conn.
 *
 * @param len [integer] number of bytes to skip
 * @return [Integer] len
 */
VALUE IO_skip(VALUE self, VALUE len) {
  struct um_io *conn = um_get_io(self);
  io_skip(conn, NUM2LONG(len), true);
  return len;
}

/* call-seq:
 *   conn.read_each { |data| } -> conn
 *
 * Reads from the target, passing each chunk to the given block.
 *
 * @return [UringMachine::IO] conn
 */
VALUE IO_read_each(VALUE self) {
  struct um_io *conn = um_get_io(self);
  io_read_each(conn);
  return self;
}

/* call-seq:
 *   conn.write(*bufs) -> len
 *
 * Writes to the connection, ensuring that all data has been written before
 * returning the total number of bytes written.
 *
 * @param bufs [Array<String, IO::Buffer>] data to write
 * @return [Integer] total bytes written
 */
VALUE IO_write(int argc, VALUE *argv, VALUE self) {
  struct um_io *conn = um_get_io(self);
  return io_writev(conn, argc, argv);
}

/* call-seq:
 *   conn.resp_read -> obj
 *
 * Decodes an object from a RESP (Redis protocol) message.
 *
 * @return [any] decoded object
 */
VALUE IO_resp_read(VALUE self) {
  struct um_io *conn = um_get_io(self);
  VALUE out_buffer = rb_utf8_str_new_literal("");
  VALUE obj = resp_read(conn, out_buffer);
  RB_GC_GUARD(out_buffer);
  return obj;
}

/* call-seq:
 *   conn.resp_write(obj) -> conn
 *
 * Writes the given object using RESP (Redis protocol) to the connection target.
 * Returns the number of bytes written.
 *
 * @param obj [any] object to write
 * @return [Integer] total bytes written
 */
VALUE IO_resp_write(VALUE self, VALUE obj) {
  struct um_io *conn = um_get_io(self);

  VALUE str = rb_str_new(NULL, 0);
  struct um_write_buffer buf;
  write_buffer_init(&buf, str);
  rb_str_modify(str);
  resp_encode(&buf, obj);
  write_buffer_update_len(&buf);

  size_t len = io_write_raw(conn, buf.ptr, buf.len);
  RB_GC_GUARD(str);
  return ULONG2NUM(len);
}

/* call-seq:
 *   conn.resp_encode(obj) -> string
 *
 * Encodes an object into a RESP (Redis protocol) message.
 *
 * @param str [String] string buffer
 * @param obj [any] object to be encoded
 * @return [String] str
 */
VALUE IO_resp_encode(VALUE self, VALUE str, VALUE obj) {
  struct um_write_buffer buf;
  write_buffer_init(&buf, str);
  rb_str_modify(str);
  resp_encode(&buf, obj);
  write_buffer_update_len(&buf);
  return str;
}

/* call-seq:
 *   conn.eof? -> bool
 *
 * Returns true if connection has reached EOF.
 *
 * @return [bool] EOF reached
 */
VALUE IO_eof_p(VALUE self) {
  struct um_io *conn = um_get_io(self);
  return conn->eof ? Qtrue : Qfalse;
}

/* call-seq:
 *   conn.consumed -> int
 *
 * Returns the total number of bytes consumed from the conn.
 *
 * @return [Integer] total bytes consumed
 */
VALUE IO_consumed(VALUE self) {
  struct um_io *conn = um_get_io(self);
  return LONG2NUM(conn->consumed_bytes);
}

/* call-seq:
 *   conn.pending -> int
 *
 * Returns the number of bytes available for reading.
 *
 * @return [Integer] bytes available
 */
VALUE IO_pending(VALUE self) {
  struct um_io *conn = um_get_io(self);
  return LONG2NUM(conn->pending_bytes);
}

/* call-seq:
 *   conn.clear -> conn
 *
 * Clears all available bytes and stops any ongoing read operation.
 *
 * @return [UM::Stream] self
 */
VALUE IO_clear(VALUE self) {
  struct um_io *conn = um_get_io(self);
  io_clear(conn);
  return self;
}

void Init_IO(void) {
  cIO = rb_define_class_under(cUM, "IO", rb_cObject);
  rb_define_alloc_func(cIO, IO_allocate);

  rb_define_method(cIO, "initialize", IO_initialize, -1);
  rb_define_method(cIO, "mode", IO_mode, 0);

  rb_define_method(cIO, "read_line", IO_read_line, 1);
  rb_define_method(cIO, "read", IO_read, 1);
  rb_define_method(cIO, "read_to_delim", IO_read_to_delim, 2);
  rb_define_method(cIO, "skip", IO_skip, 1);
  rb_define_method(cIO, "read_each", IO_read_each, 0);

  rb_define_method(cIO, "write", IO_write, -1);

  rb_define_method(cIO, "resp_read", IO_resp_read, 0);
  rb_define_method(cIO, "resp_write", IO_resp_write, 1);
  rb_define_singleton_method(cIO, "resp_encode", IO_resp_encode, 2);

  rb_define_method(cIO, "eof?", IO_eof_p, 0);
  rb_define_method(cIO, "consumed", IO_consumed, 0);
  rb_define_method(cIO, "pending", IO_pending, 0);
  rb_define_method(cIO, "clear", IO_clear, 0);

  eIORESPError = rb_define_class_under(cIO, "RESPError", rb_eStandardError);

  SYM_fd = ID2SYM(rb_intern("fd"));
  SYM_socket = ID2SYM(rb_intern("socket"));
  SYM_ssl     = ID2SYM(rb_intern("ssl"));
}
