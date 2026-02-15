#include "um.h"
#include <arpa/inet.h>
#include <ruby/io.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/inotify.h>

VALUE cUM;
VALUE eUMError;

VALUE SYM_size;
VALUE SYM_total_ops;
VALUE SYM_total_switches;
VALUE SYM_total_waits;
VALUE SYM_ops_pending;
VALUE SYM_ops_unsubmitted;
VALUE SYM_ops_runqueue;
VALUE SYM_ops_free;
VALUE SYM_ops_transient;
VALUE SYM_time_total_cpu;
VALUE SYM_time_total_wait;

VALUE SYM_wd;
VALUE SYM_mask;
VALUE SYM_name;

static ID id_fileno;

static void UM_mark(void *ptr) {
  struct um *machine = ptr;
  rb_gc_mark_movable(machine->self);
  rb_gc_mark_movable(machine->pending_fibers);

  um_op_list_mark(machine, machine->transient_head);
  um_op_list_mark(machine, machine->runqueue_head);
}

static void UM_compact(void *ptr) {
  struct um *machine = ptr;
  machine->self = rb_gc_location(machine->self);
  machine->pending_fibers = rb_gc_location(machine->pending_fibers);

  um_op_list_compact(machine, machine->transient_head);
  um_op_list_compact(machine, machine->runqueue_head);
}

static void UM_free(void *ptr) {
  um_teardown((struct um *)ptr);
  free(ptr);
}

static const rb_data_type_t UringMachine_type = {
  .wrap_struct_name = "UringMachine",
  .function = {
    .dmark = UM_mark,
    .dfree = UM_free,
    .dsize = NULL,
    .dcompact = UM_compact
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE UM_allocate(VALUE klass) {
  struct um *um;
  return TypedData_Make_Struct(klass, struct um, &UringMachine_type, um);
}

inline struct um *um_get_machine(VALUE self) {
  struct um *um;
  TypedData_Get_Struct(self, struct um, &UringMachine_type, um);
  if (!um->ring_initialized) um_raise_internal_error("Machine not initialized");

  return um;
}

static inline uint get_sqpoll_timeout_msec(VALUE sqpoll_timeout) {
  switch (TYPE(sqpoll_timeout)) {
    case T_NIL:
    case T_UNDEF:
    case T_FALSE:
      return 0;
    case T_FLOAT:
      return (uint)(NUM2DBL(sqpoll_timeout) * 1000);
    case T_FIXNUM:
      return NUM2UINT(sqpoll_timeout) * 1000;
    case T_TRUE:
      return 1000;
    default:
      rb_raise(eUMError, "Invalid sqpoll_timeout value");
  }
}

/* Initializes a new UringMachine instance with the given options.
 *
 * @overload initialize(size: 4096, sqpoll: false, sidecar: false)
 *   @param size [Integer] SQ size (default: 4096)
 *   @param sqpoll [bool, Number] Set SQPOLL mode, SQPOLL timeout
 *   @param sidecar [bool] Set sidecar mode
 *   @return [void]
 */
VALUE UM_initialize(int argc, VALUE *argv, VALUE self) {
  static ID kwargs_ids[3];
  struct um *machine = RTYPEDDATA_DATA(self);
  VALUE opts, kwargs[3] = {Qnil, Qnil, Qnil};

  if (!kwargs_ids[0]) {
    kwargs_ids[0] = rb_intern_const("size");
    kwargs_ids[1] = rb_intern_const("sqpoll");
    kwargs_ids[2] = rb_intern_const("sidecar");
  }
  rb_scan_args(argc, argv, "0:", &opts);
  if (!NIL_P(opts)) {
    rb_get_kwargs(opts, kwargs_ids, 0, 3, kwargs);
  }

  uint entries_i = TYPE(kwargs[0]) == T_FIXNUM ? NUM2UINT(kwargs[0]) : 0;
  uint sqpoll_timeout_msec = get_sqpoll_timeout_msec(kwargs[1]);
  um_setup(self, machine, entries_i, sqpoll_timeout_msec, RTEST(kwargs[2]));

  return self;
}

/* Creates a buffer group (buffer ring) with the given buffer size and buffer count.
 *
 * @param size [Integer] buffer size in bytes
 * @param count [Integer] number of buffers in group
 * @return [Integer] buffer group id
 */
VALUE UM_setup_buffer_ring(VALUE self, VALUE size, VALUE count) {
  struct um *machine = um_get_machine(self);
  int bgid = um_setup_buffer_ring(machine, NUM2UINT(size), NUM2UINT(count));
  return INT2NUM(bgid);
}

/* Returns the SQ (submission queue) size.
 *
 * @return [Integer] SQ size
 */
VALUE UM_size(VALUE self) {
  struct um *machine = um_get_machine(self);
  return UINT2NUM(machine->size);
}

/* :nodoc:
 */
VALUE UM_mark_m(VALUE self, VALUE mark) {
  struct um *machine = um_get_machine(self);
  machine->mark = NUM2UINT(mark);
  return self;
}

/* Returns a hash with different metrics about the functioning of the
 * UringMachine instance.
 *
 * @return [Hash] metrics hash
 */
VALUE UM_metrics(VALUE self) {
  struct um *machine = um_get_machine(self);
  return um_metrics(machine, &machine->metrics);
}

/* Returns the profile mode state.
 *
 * @return [bool] profile mode on/off
 */
VALUE UM_profile_mode_p(VALUE self) {
  struct um *machine = um_get_machine(self);
  return machine->profile_mode ? Qtrue : Qfalse;
}

/* Returns the SQPOLL mode state.
 *
 * @return [bool] SQPOLL mode on/off
 */
VALUE UM_sqpoll_mode_p(VALUE self) {
  struct um *machine = um_get_machine(self);
  return machine->sqpoll_mode ? Qtrue : Qfalse;
}

/* Sets/resets profile mode.
 *
 * @param value [bool] profile mode on/off
 * @return [bool] profile mode on/off
 */
VALUE UM_profile_mode_set(VALUE self, VALUE value) {
  struct um *machine = um_get_machine(self);
  machine->profile_mode = RTEST(value);
  if (machine->profile_mode) {
    machine->metrics.time_total_wait = 0.0;
    machine->metrics.time_last_cpu = machine->metrics.time_first_cpu = um_get_time_cpu();
  }
  return value;
}

/* Sets/resets test mode.
 *
 * @param value [bool] test mode on/off
 * @return [bool] test mode on/off
 */
VALUE UM_test_mode_set(VALUE self, VALUE value) {
  struct um *machine = um_get_machine(self);
  machine->test_mode = RTEST(value);
  return value;
}

/* Returns the sidecar mode state.
 *
 * @return [bool] sidecar mode on/off
 */
VALUE UM_sidecar_mode_p(VALUE self) {
  struct um *machine = um_get_machine(self);
  return machine->sidecar_mode ? Qtrue : Qfalse;
}

/* Starts sidecar mode.
 *
 * @return [UringMachine] self
 */
VALUE UM_sidecar_start(VALUE self) {
  struct um *machine = um_get_machine(self);
  um_sidecar_setup(machine);
  return self;
}

/* Stops sidecar mode.
 *
 * @return [UringMachine] self
 */
VALUE UM_sidecar_stop(VALUE self) {
  struct um *machine = um_get_machine(self);
  um_sidecar_teardown(machine);
  return self;
}

/* Adds the current fiber to the end of the runqueue and yields control to the
 * next fiber in the runqueue. This method is usually used to yield control
 * while performing CPU-intensive work in order not starve other fibers.
 *
 * @return [UringMachine] self
 */
VALUE UM_snooze(VALUE self) {
  struct um *machine = um_get_machine(self);
  um_schedule(machine, rb_fiber_current(), Qnil);

  // the current fiber is already scheduled, and the runqueue is GC-marked, so
  // we can safely call um_switch, which is faster than calling um_yield.
  VALUE ret = um_switch(machine);
  RAISE_IF_EXCEPTION(ret);
  return ret;
}

/* Yields control to the next fiber in the runqueue. The current fiber will not
 * be resumed unless it is scheduled again by some other fiber. The call to
 * `#yield` will return the value with which the current fiber will be
 * eventually scheduled.
 *
 * @return [any] resume value
 */
VALUE UM_yield(VALUE self) {
  struct um *machine = um_get_machine(self);

  VALUE ret = um_yield(machine);
  RAISE_IF_EXCEPTION(ret);
  return ret;
}

/* Yields control to the next fiber in the runqueue. The current fiber will not
 * be resumed unless it is scheduled again by some other fiber. The call to
 * `#yield` will return the value with which the current fiber will be
 * eventually scheduled. If resumed with an exception, that exception will be
 * raised when the fiber is resumed.
 *
 * @return [any] resume value
 */
VALUE UM_switch(VALUE self) {
  struct um *machine = um_get_machine(self);

  VALUE ret = um_switch(machine);
  RAISE_IF_EXCEPTION(ret);
  return ret;
}

/* Wakes up the machine in order to start processing its runqueue again. This
 * method is normally called from another thread in order to resume processing
 * of the runqueue when a machine is waiting for I/O completions.
 *
 * @return [void]
 */
VALUE UM_wakeup(VALUE self) {
  struct um *machine = um_get_machine(self);
  return um_wakeup(machine);
}

/* Submits any pending I/O operations that are not yet submitted.
 *
 * @return [Integer] number of I/O operations submitted
 */
VALUE UM_submit(VALUE self) {
  struct um *machine = um_get_machine(self);
  uint ret = um_submit(machine);
  return UINT2NUM(ret);
}

/* Returns a set containing all fibers in pending state (i.e. waiting for an
 * operation to complete.)
 *
 * @return [Set] set of pending fibers
 */
VALUE UM_pending_fibers(VALUE self) {
  struct um *machine = um_get_machine(self);
  return machine->pending_fibers;
}

/* Schedules the given fiber by adding it to the runqueue. The fiber will be
 * resumed with the given value.
 *
 * @param fiber [Fiber] fiber to schedule
 * @param value [any] resume value
 * @return [UringMachine] self
 */
VALUE UM_schedule(VALUE self, VALUE fiber, VALUE value) {
  struct um *machine = um_get_machine(self);
  um_schedule(machine, fiber, value);
  return self;
}

/* Runs the given block, interrupting its execution if its runtime exceeds the
 * given timeout interval (in seconds).
 *
 * - https://www.man7.org/linux/man-pages//man3/io_uring_prep_timeoute.3.html
 *
 * @param interval [Number] timeout interval in seconds
 * @param exception_class [any] timeout exception class
 * @return [any] block's return value
 */
VALUE UM_timeout(VALUE self, VALUE interval, VALUE exception_class) {
  struct um *machine = um_get_machine(self);
  return um_timeout(machine, interval, exception_class);
}

/* Puts the current fiber to sleep for the given time duration (in seconds),
 * yielding control to the next fiber in the runqueue.
 *
 * - https://www.man7.org/linux/man-pages//man3/io_uring_prep_timeoute.3.html
 *
 * @param duration [Number] sleep duration in seconds
 * @return [void]
 */
VALUE UM_sleep(VALUE self, VALUE duration) {
  struct um *machine = um_get_machine(self);
  return um_sleep(machine, NUM2DBL(duration));
}

/* Runs the given block at regular time intervals in an infinite loop.
 *
 * - https://www.man7.org/linux/man-pages//man3/io_uring_prep_timeoute.3.html
 *
 * @param interval [Number] time interval (in seconds) between consecutive invocations
 * @return [void]
 */
VALUE UM_periodically(VALUE self, VALUE interval) {
  struct um *machine = um_get_machine(self);
  return um_periodically(machine, NUM2DBL(interval));
}

/* call-seq:
 *   machine.read(fd, buffer, maxlen, buffer_offset = nil, file_offset = nil) -> bytes_read
 *
 * Reads up to `maxlen` bytes from the given `fd` into the given buffer. The
 * optional `buffer_offset` parameter determines the position in the buffer into
 * which the data will be read. A negative `buffer_offset` denotes a position
 * relative to the end of the buffer, e.g. a value of `-1` means the data will
 * be appended to the buffer.
 * 
 * - https://www.man7.org/linux/man-pages/man2/read.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_read.3.html
 *
 * @param fd [Integer] file descriptor
 * @param buffer [String, IO::Buffer] buffer
 * @param maxlen [Integer] maximum number of bytes to read
 * @param buffer_offset [Integer] optional buffer offset to read into
 * @param file_offset [Integer] optional file offset to read from
 * @return [Integer] number of bytes read
 */
VALUE UM_read(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  VALUE fd;
  VALUE buffer;
  VALUE maxlen;
  VALUE buffer_offset;
  VALUE file_offset;
  rb_scan_args(argc, argv, "32", &fd, &buffer, &maxlen, &buffer_offset, &file_offset);

  ssize_t maxlen_i = NIL_P(maxlen) ? -1 : NUM2INT(maxlen);
  ssize_t buffer_offset_i = NIL_P(buffer_offset) ? 0 : NUM2INT(buffer_offset);
  __u64 file_offset_i = NIL_P(file_offset) ? (__u64)-1 : NUM2UINT(file_offset);

  return um_read(machine, NUM2INT(fd), buffer, maxlen_i, buffer_offset_i, file_offset_i);
}

/* call-seq:
 *   machine.read_each(fd, bgid) { |data| }
 *
 * Reads repeatedly from the given `fd` using the given buffer group id. The
 * buffer group should have been previously setup using `#setup_buffer_ring`.
 * Read data is yielded in an infinite loop to the given block.
 *
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_read_multishot.3.html
 *
 * @param fd [Integer] file descriptor
 * @param bgid [Integer] buffer group id
 * @return [void]
 */
VALUE UM_read_each(VALUE self, VALUE fd, VALUE bgid) {
  struct um *machine = um_get_machine(self);
  return um_read_each(machine, NUM2INT(fd), NUM2INT(bgid));
}

/* call-seq:
 *   machine.write(fd, buffer, len = nil, file_offset = nil) -> bytes_written
 *
 * Writes up to `len` bytes from the given buffer to the given `fd`. If `len`,
 * is not given, the entire buffer length is used.
 *
 * - https://man7.org/linux/man-pages/man2/write.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_write.3.html
 *
 * @param fd [Integer] file descriptor
 * @param buffer [String, IO::Buffer] buffer
 * @param len [Integer] maximum number of bytes to write
 * @param file_offset [Integer] optional file offset to write to
 * @return [Integer] number of bytes written
 */
VALUE UM_write(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  VALUE fd;
  VALUE buffer;
  VALUE len;
  VALUE file_offset;
  rb_scan_args(argc, argv, "22", &fd, &buffer, &len, &file_offset);

  size_t len_i = NIL_P(len) ? (size_t)-1 : NUM2UINT(len);
  __u64 file_offset_i = NIL_P(file_offset) ? (__u64)-1 : NUM2UINT(file_offset);

  return um_write(machine, NUM2INT(fd), buffer, len_i, file_offset_i);
}

/* call-seq:
 *   machine.writev(fd, *buffers, file_offset = nil) -> bytes_written
 *
 * Writes from the given buffers into the given fd. This method does not
 * guarantee that all data will be written. The application code should check
 * the return value which indicates the number of bytes written and potentially
 * repeat the operation after adjusting the buffers accordingly. See also
 * `#sendv`.
 *
 * - https://man7.org/linux/man-pages/man2/writev.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_writev.3.html
 *
 * @param fd [Integer] file descriptor
 * @param *buffers [Array<String, IO::Buffer>] data buffers
 * @param file_offset [Integer] optional file offset to write to
 * @return [Integer] number of bytes written
 */
VALUE UM_writev(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  if (argc < 1)
    rb_raise(rb_eArgError, "wrong number of arguments (given 0, expected 1+)");
  int fd = NUM2INT(argv[0]);
  if (argc < 2) return INT2NUM(0);

  return um_writev(machine, fd, argc - 1, argv + 1);
}

/* call-seq:
 *   machine.write_async(fd, buffer, len = nil, file_offset = nil) -> buffer
 *
 * Writes up to `len` bytes from the given buffer to the given `fd`. If `len`,
 * is not given, the entire buffer length is used. This method submits the
 * operation but does not wait for it to complete. This method may be used to
 * improve performance in situations where the application does not care about
 * whether the I/O operation succeeds or not.
 *
 * - https://man7.org/linux/man-pages/man2/write.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_write.3.html
 *
 * @param fd [Integer] file descriptor
 * @param buffer [String, IO::Buffer] buffer
 * @param len [Integer] maximum number of bytes to write
 * @param file_offset [Integer] optional file offset to write to
 * @return [String, IO::Buffer] buffer
 */
VALUE UM_write_async(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  VALUE fd;
  VALUE buffer;
  VALUE len;
  VALUE file_offset;
  rb_scan_args(argc, argv, "22", &fd, &buffer, &len, &file_offset);

  size_t len_i = NIL_P(len) ? (size_t)-1 : NUM2UINT(len);
  __u64 file_offset_i = NIL_P(file_offset) ? (__u64)-1 : NUM2UINT(file_offset);

  return um_write_async(machine, NUM2INT(fd), buffer, len_i, file_offset_i);
}

/* call-seq:
 *   machine.statx(fd, nil, flags, mask) -> hash
 *   machine.statx(dirfd, path, flags, mask) -> hash
 *   machine.statx(UM::AT_FDCWD, path, flags, mask) -> hash
 *
 * Returns information about a file.
 *
 * - https://www.man7.org/linux/man-pages/man2/statx.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_statx.3.html
 *
 * @param dirfd [Integer] file or directory descriptor
 * @param path [String, nil] file path
 * @param flags [Integer] flags
 * @param mask [Integer] mask of information to return
 * @return [hash] hash containing file information
 */
VALUE UM_statx(VALUE self, VALUE dirfd, VALUE path, VALUE flags, VALUE mask) {
  struct um *machine = um_get_machine(self);
  return um_statx(machine, NUM2INT(dirfd), path, NUM2INT(flags), NUM2UINT(mask));
}

/* call-seq:
 *   machine.close(fd) -> 0
 *
 * Closes the given file descriptor.
 *
 * - https://www.man7.org/linux/man-pages/man2/close.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_close.3.html
 *
 * @param fd [Integer] file descriptor
 * @return [0] success
 */
VALUE UM_close(VALUE self, VALUE fd) {
  struct um *machine = um_get_machine(self);
  return um_close(machine, NUM2INT(fd));
}

/* call-seq:
 *   machine.close_async(fd) -> hash
 *
 * Closes the given file descriptor. This method submits the operation but does
 * not wait for it to complete. This method may be used to improve performance
 * in cases where the application des not care whether it succeeds or not.
 *
 * - https://www.man7.org/linux/man-pages/man2/close.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_close.3.html
 *
 * @param fd [Integer] file descriptor
 * @return [Integer] file descriptor
 */
VALUE UM_close_async(VALUE self, VALUE fd) {
  struct um *machine = um_get_machine(self);
  return um_close_async(machine, NUM2INT(fd));
}

/* call-seq:
 *   machine.accept(server_fd) -> connection_fd
 *
 * Accepts an incoming TCP connection.
 *
 * - https://www.man7.org/linux/man-pages/man2/accept4.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_accept.3.html
 *
 * @param server_fd [Integer] listening socket file descriptor
 * @return [Integer] connection file descriptor
 */
VALUE UM_accept(VALUE self, VALUE server_fd) {
  struct um *machine = um_get_machine(self);
  return um_accept(machine, NUM2INT(server_fd));
}

/* call-seq:
 *   machine.accept_each(server_fd) { |connection_fd| ... }
 *
 * Repeatedly accepts incoming TCP connections in a loop, yielding the
 * connection file descriptors to the given block.
 *
 * - https://www.man7.org/linux/man-pages/man2/accept4.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_accept.3.html
 *
 * @param server_fd [Integer] listening socket file descriptor
 * @return [void]
 */
VALUE UM_accept_each(VALUE self, VALUE server_fd) {
  struct um *machine = um_get_machine(self);
  return um_accept_each(machine, NUM2INT(server_fd));
}

/* call-seq:
 *   machine.accept_into_queue(server_fd, queue)
 *
 * Repeatedly accepts incoming TCP connections in a loop, pushing the connection
 * file descriptors into the given queue.
 *
 * - https://www.man7.org/linux/man-pages/man2/accept4.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_accept.3.html
 *
 * @param server_fd [Integer] listening socket file descriptor
 * @param queue [UM::Queue] connection queue
 * @return [void]
 */
VALUE UM_accept_into_queue(VALUE self, VALUE server_fd, VALUE queue) {
  struct um *machine = um_get_machine(self);
  return um_accept_into_queue(machine, NUM2INT(server_fd), queue);
}

/* call-seq:
 *   machine.socket(domain, type, protocol, flags) -> fd
 *
 * Creates a socket.
 *
 * - https://www.man7.org/linux/man-pages/man2/socket.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_socket.3.html
 *
 * @param domain [Integer] socket domain
 * @param type [Integer] socket type
 * @param protocol [Integer] socket protocol
 * @param flags [Integer] flags
 * @return [Integer] file descriptor
 */
VALUE UM_socket(VALUE self, VALUE domain, VALUE type, VALUE protocol, VALUE flags) {
  struct um *machine = um_get_machine(self);
  return um_socket(machine, NUM2INT(domain), NUM2INT(type), NUM2INT(protocol), NUM2UINT(flags));
}

/* call-seq:
 *   machine.shutdown(fd, how) -> 0
 *
 * Shuts down a socket for sending and/or receiving.
 *
 * - https://man7.org/linux/man-pages/man2/shutdown.2.html
 * - https://man7.org/linux/man-pages/man3/io_uring_prep_shutdown.3.html
 *
 * @param fd [Integer] file descriptor
 * @param how [Integer] how the socket should be shutdown
 * @return [0] success
 */
VALUE UM_shutdown(VALUE self, VALUE fd, VALUE how) {
  struct um *machine = um_get_machine(self);
  return um_shutdown(machine, NUM2INT(fd), NUM2INT(how));
}

/* call-seq:
 *   machine.shutdown_async(fd, how) -> 0
 *
 * Shuts down a socket for sending and/or receiving. This method may be used to
 * improve performance in situations where the application does not care about
 * whether the operation succeeds or not.
 *
 * - https://man7.org/linux/man-pages/man2/shutdown.2.html
 * - https://man7.org/linux/man-pages/man3/io_uring_prep_shutdown.3.html
 *
 * @param fd [Integer] file descriptor
 * @param how [Integer] how the socket should be shutdown
 * @return [0] success
 */
VALUE UM_shutdown_async(VALUE self, VALUE fd, VALUE how) {
  struct um *machine = um_get_machine(self);
  return um_shutdown_async(machine, NUM2INT(fd), NUM2INT(how));
}

/* call-seq:
 *   machine.send_fd(sock_fd, fd) -> fd
 *
 * Sends the given file descriptor over the given socket.
 *
 * - https://www.man7.org/linux/man-pages/man3/sendmsg.3p.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_sendmsg.3.html
 *
 * @param sock_fd [Integer] socket file descriptor
 * @param fd [Integer] file descriptor to send
 * @return [Integer] file descriptor to send
 */
VALUE UM_send_fd(VALUE self, VALUE sock_fd, VALUE fd) {
  struct um *machine = um_get_machine(self);
  return um_send_fd(machine, NUM2INT(sock_fd), NUM2INT(fd));
}

/* call-seq:
 *   machine.recv_fd(sock_fd) -> fd
 *
 * Receives a file descriptor over the given socket.
 *
 * - https://www.man7.org/linux/man-pages/man3/recvmsg.3p.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_recvmsg.3.html
 *
 * @param sock_fd [Integer] socket file descriptor
 * @return [Integer] rececived file descriptor
 */
VALUE UM_recv_fd(VALUE self, VALUE sock_fd) {
  struct um *machine = um_get_machine(self);
  return um_recv_fd(machine, NUM2INT(sock_fd));
}

/* call-seq:
 *   machine.connect(fd, host, port) -> 0
 *
 * Connects the given socket to the given host and port.
 *
 * - https://www.man7.org/linux/man-pages/man2/connect.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_connect.3.html
 *
 * @param fd [Integer] file descriptor
 * @param host [String] hostname or IP address
 * @param port [Integer] port number
 * @return [0] success
 */
VALUE UM_connect(VALUE self, VALUE fd, VALUE host, VALUE port) {
  struct um *machine = um_get_machine(self);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(StringValueCStr(host));
  addr.sin_port = htons(NUM2INT(port));

  return um_connect(machine, NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
}

/* call-seq:
 *   machine.send(fd, buffer, len, flags) -> bytes_sent
 *
 * Sends data on the given socket. This method is not guaranteed to send all of
 * the data in the buffer, unless `UM::MSG_WAITALL` is specified in the flags
 * mask.
 *
 * - https://www.man7.org/linux/man-pages/man2/send.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_send.3.html
 *
 * @param fd [Integer] file descriptor
 * @param buffer [String, IO::Buffer] buffer
 * @param len [Integer] number of bytes to send
 * @param flags [Integer] flags mask
 * @return [Integer] number of bytes sent
 */
VALUE UM_send(VALUE self, VALUE fd, VALUE buffer, VALUE len, VALUE flags) {
  struct um *machine = um_get_machine(self);
  return um_send(machine, NUM2INT(fd), buffer, NUM2INT(len), NUM2INT(flags));
}

#ifdef HAVE_IO_URING_SEND_VECTORIZED

/* call-seq:
 *   machine.sendv(fd, *buffers) -> bytes_sent
 *
 * Sends data on the given socket from the given buffers. This method is only
 * available on Linux kernel >= 6.17. This method is guaranteed to send
 *
 * - https://www.man7.org/linux/man-pages/man2/send.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_send.3.html
 *
 * @overload sendv(fd, *buffers)
 *   @param fd [Integer] file descriptor
 *   @param *buffers [Array<String, IO::Buffer>] buffers
 *   @return [Integer] number of bytes sent
 */
VALUE UM_sendv(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  if (argc < 1)
    rb_raise(rb_eArgError, "wrong number of arguments (given 0, expected 1+)");
  int fd = NUM2INT(argv[0]);
  if (argc < 2) return INT2NUM(0);

  return um_sendv(machine, fd, argc - 1, argv + 1);
}

#endif

/* call-seq:
 *   machine.send_bundle(fd, bgid, *buffers) -> bytes_sent
 *
 * Sends data on the given socket from the given buffers using a registered
 * buffer group. The buffer group should have been previously registered using
 * `#setup_buffer_ring`.
 *
 * - https://www.man7.org/linux/man-pages/man2/send.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_send.3.html
 *
 * @overload send_bundle(fd, bgid, *buffers)
 *   @param fd [Integer] file descriptor
 *   @param bgid [Integer] buffer group id
 *   @param *buffers [Array<String, IO::Buffer>] buffers
 *   @return [Integer] number of bytes sent
 */
VALUE UM_send_bundle(int argc, VALUE *argv, VALUE self) {
  struct um *machine = um_get_machine(self);
  VALUE fd;
  VALUE bgid;
  VALUE strings;
  rb_scan_args(argc, argv, "2*", &fd, &bgid, &strings);

  if (RARRAY_LEN(strings) == 1) {
    VALUE first = rb_ary_entry(strings, 0);
    if (TYPE(first) == T_ARRAY)
      strings = first;
  }

  return um_send_bundle(machine, NUM2INT(fd), NUM2INT(bgid), strings);
}

/* call-seq:
 *   machine.recv(fd, buffer, maxlen, flags) -> bytes_received
 *
 * Receives data from the given socket.
 *
 * - https://www.man7.org/linux/man-pages/man2/recv.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_recv.3.html
 *
 * @param fd [Integer] file descriptor
 * @param buffer [String, IO::Buffer] buffer
 * @param maxlen [Integer] maximum number of bytes to receive
 * @param flags [Integer] flags mask
 * @return [Integer] number of bytes received
 */
VALUE UM_recv(VALUE self, VALUE fd, VALUE buffer, VALUE maxlen, VALUE flags) {
  struct um *machine = um_get_machine(self);
  return um_recv(machine, NUM2INT(fd), buffer, NUM2INT(maxlen), NUM2INT(flags));
}

/* call-seq:
 *   machine.recv_each(fd, bgid, flags) { |data| ... }
 *
 * Repeatedlty receives data from the given socket in an infinite loop using the
 * given buffer group id. The buffer group should have been previously setup
 * using `#setup_buffer_ring`.
 *
 * - https://www.man7.org/linux/man-pages/man2/recv.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_recv.3.html
 *
 * @param fd [Integer] file descriptor
 * @param bgid [Integer] buffer group id
 * @param flags [Integer] flags mask
 * @return [void]
 */
VALUE UM_recv_each(VALUE self, VALUE fd, VALUE bgid, VALUE flags) {
  struct um *machine = um_get_machine(self);
  return um_recv_each(machine, NUM2INT(fd), NUM2INT(bgid), NUM2INT(flags));
}

/* call-seq:
 *   machine.bind(fd, host, port) -> 0
 *
 * Binds the given socket to the given host and port.
 *
 * - https://www.man7.org/linux/man-pages/man2/bind.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_bind.3.html
 *
 * @param fd [Integer] file descriptor
 * @param host [String] hostname or IP address
 * @param port [Integer] port number
 * @return [0] success
 */
VALUE UM_bind(VALUE self, VALUE fd, VALUE host, VALUE port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(StringValueCStr(host));
  addr.sin_port = htons(NUM2INT(port));

#ifdef HAVE_IO_URING_PREP_BIND
  struct um *machine = um_get_machine(self);
  return um_bind(machine, NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
#else
  int res = bind(NUM2INT(fd), (struct sockaddr *)&addr, sizeof(addr));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  return INT2NUM(0);
#endif
}

/* call-seq:
 *   machine.listen(fd, backlog) -> 0
 *
 * Starts listening for incoming connections on the given socket.
 *
 * - https://www.man7.org/linux/man-pages/man2/listen.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_listen.3.html
 *
 * @param fd [Integer] file descriptor
 * @param backlog [String] pending connection queue length
 * @return [0] success
 */
VALUE UM_listen(VALUE self, VALUE fd, VALUE backlog) {
#ifdef HAVE_IO_URING_PREP_LISTEN
  struct um *machine = um_get_machine(self);
  return um_listen(machine, NUM2INT(fd), NUM2INT(backlog));
#else
  int res = listen(NUM2INT(fd), NUM2INT(backlog));
  if (res)
    rb_syserr_fail(errno, strerror(errno));
  return INT2NUM(0);
#endif
}

static inline int numeric_value(VALUE value) {
  switch (TYPE(value)) {
    case T_TRUE:
      return 1;
    case T_FALSE:
      return 0;
    default:
      return NUM2INT(value);
  }
}

/* call-seq:
 *   machine.getsockopt(fd, level, opt) -> value
 *
 * Returns the value of a socket option.
 *
 * - https://www.man7.org/linux/man-pages//man2/getsockopt.2.html
 * - https://www.man7.org/linux/man-pages//man3/io_uring_prep_cmd.3.html
 *
 * @param fd [Integer] file descriptor
 * @param level [Integer] level
 * @param opt [Integer] level
 * @return [Integer] option value
 */
VALUE UM_getsockopt(VALUE self, VALUE fd, VALUE level, VALUE opt) {
  struct um *machine = um_get_machine(self);
  return um_getsockopt(machine, NUM2INT(fd), NUM2INT(level), NUM2INT(opt));
}

/* call-seq:
 *   machine.setsockopt(fd, level, opt, value) -> 0
 *
 * Sets the value of a socket option.
 *
 * - https://www.man7.org/linux/man-pages//man2/setsockopt.2.html
 * - https://www.man7.org/linux/man-pages//man3/io_uring_prep_cmd.3.html
 *
 * @param fd [Integer] file descriptor
 * @param level [Integer] level
 * @param opt [Integer] level
 * @param value [Integer] option value
 * @return [0] success
 */
VALUE UM_setsockopt(VALUE self, VALUE fd, VALUE level, VALUE opt, VALUE value) {
  struct um *machine = um_get_machine(self);
  return um_setsockopt(machine, NUM2INT(fd), NUM2INT(level), NUM2INT(opt), numeric_value(value));
}

/* call-seq:
 *   machine.synchronize(mutex) { }
 *
 * Synchronizes access to the given mutex. The mutex is locked, the given block
 * is executed and finally the mutex is unlocked.
 *
 * - https://www.man7.org/linux/man-pages/man2/futex.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_futex_wait.3.html
 *
 * @param mutex [UM::Mutex] mutex
 * @return [any] block return value
 */
VALUE UM_mutex_synchronize(VALUE self, VALUE mutex) {
  struct um *machine = um_get_machine(self);
  struct um_mutex *mutex_data = Mutex_data(mutex);
  return um_mutex_synchronize(machine, mutex_data);
}

/* call-seq:
 *   machine.push(queue, value) -> queue
 *
 * Pushes a value to the tail of the given queue.
 *
 * - https://www.man7.org/linux/man-pages/man2/futex.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_futex_wait.3.html
 *
 * @param queue [UM::Queue] queue
 * @param value [any] value
 * @return [UM::Queue] queue
 */
VALUE UM_queue_push(VALUE self, VALUE queue, VALUE value) {
  struct um *machine = um_get_machine(self);
  struct um_queue *que = Queue_data(queue);
  return um_queue_push(machine, que, value);
}

/* call-seq:
 *   machine.pop(queue) -> value
 *
 * removes a value from the tail of the given queue.
 *
 * - https://www.man7.org/linux/man-pages/man2/futex.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_futex_wait.3.html
 *
 * @param queue [UM::Queue] queue
 * @return [any] value
 */
VALUE UM_queue_pop(VALUE self, VALUE queue) {
  struct um *machine = um_get_machine(self);
  struct um_queue *que = Queue_data(queue);
  return um_queue_pop(machine, que);
}

/* call-seq:
 *   machine.unshift(queue, value) -> queue
 *
 * Pushes a value to the head of the given queue.
 *
 * - https://www.man7.org/linux/man-pages/man2/futex.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_futex_wait.3.html
 *
 * @param queue [UM::Queue] queue
 * @param value [any] value
 * @return [UM::Queue] queue
 */
VALUE UM_queue_unshift(VALUE self, VALUE queue, VALUE value) {
  struct um *machine = um_get_machine(self);
  struct um_queue *que = Queue_data(queue);
  return um_queue_unshift(machine, que, value);
}

/* call-seq:
 *   machine.pop(queue) -> value
 *
 * removes a value from the head of the given queue.
 *
 * - https://www.man7.org/linux/man-pages/man2/futex.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_futex_wait.3.html
 *
 * @param queue [UM::Queue] queue
 * @return [any] value
 */
VALUE UM_queue_shift(VALUE self, VALUE queue) {
  struct um *machine = um_get_machine(self);
  struct um_queue *que = Queue_data(queue);
  return um_queue_shift(machine, que);
}

struct um_open_ctx {
  VALUE self;
  VALUE fd;
};

VALUE UM_open_complete(VALUE arg) {
  struct um_open_ctx *ctx = (struct um_open_ctx *)arg;
  UM_close(ctx->self, ctx->fd);
  return ctx->self;
}

/* call-seq:
 *   machine.open(pathname, flags) -> fd
 *   machine.open(pathname, flags) { |fd| ... }
 *
 * Opens a file using the given pathname and flags. If a block is given, the
 * file descriptor is passed to the block, and the file is automatically closed
 * when the block returns.
 *
 * - https://www.man7.org/linux/man-pages/man2/open.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_openat.3.html
 *
 * @param pathname [String] file path
 * @param flags [Integer] flags mask
 * @return [Integer] fd
 */
VALUE UM_open(VALUE self, VALUE pathname, VALUE flags) {
  struct um *machine = um_get_machine(self);
  // TODO: take optional perm (mode) arg
  VALUE fd = um_open(machine, pathname, NUM2INT(flags), 0666);
  if (rb_block_given_p()) {
    struct um_open_ctx ctx = { self, fd };
    return rb_ensure(rb_yield, fd, UM_open_complete, (VALUE)&ctx);
  }
  else
    return fd;
}

/* call-seq:
 *   machine.poll(fd, mask) -> fd
 *
 * Waits for readiness of the given file descriptor according to the given event
 * mask.
 *
 * - https://www.man7.org/linux/man-pages/man2/poll.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_poll_add.3.html
 *
 * @param fd [Integer] file descriptor
 * @param mask [Integer] events mask
 * @return [Integer] fd
 */
VALUE UM_poll(VALUE self, VALUE fd, VALUE mask) {
  struct um *machine = um_get_machine(self);
  return um_poll(machine, NUM2INT(fd), NUM2UINT(mask));
}

/* call-seq:
 *   machine.select(read_fds, write_fds, except_fds) -> [read_ready_fds, write_read_fds, except_ready_fds]
 *
 * Waits for readyness of at least one fd for read, write or exception condition
 * from the given fds. This method provides a similar interface to `IO#select`.
 *
 * - https://www.man7.org/linux/man-pages/man2/select.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_poll_add.3.html
 *
 * @param read_fds [Array<Integer>] file descriptors for reading
 * @param write_fds [Array<Integer>] file descriptors for writing
 * @param except_fds [Array<Integer>] file descriptors for exception
 * @return [Array<Array<Integer>>] read-ready, write-ready, and exception-ready fds
 */
VALUE UM_select(VALUE self, VALUE read_fds, VALUE write_fds, VALUE except_fds) {
  struct um *machine = um_get_machine(self);
  return um_select(machine, read_fds, write_fds, except_fds);
}

/* call-seq:
 *   machine.waitid(idtype, id, options) -> [pid, status, code)]
 *
 * Waits for a process to change state. The process to wait for can be specified
 * as a pid or as a pidfd, according to the given `idtype` and `id`.
 *
 * - https://www.man7.org/linux/man-pages/man2/waitid.2.html
 * - https://www.man7.org/linux/man-pages/man3/io_uring_prep_waitid.3.html
 *
 * @param idtype [Integer] id type
 * @param id [Integer] id
 * @param options [Integer] options
 * @return [Array<Integer>] pid status
 */
VALUE UM_waitid(VALUE self, VALUE idtype, VALUE id, VALUE options) {
  struct um *machine = um_get_machine(self);
  return um_waitid(machine, NUM2INT(idtype), NUM2INT(id), NUM2INT(options));
}

#ifdef HAVE_RB_PROCESS_STATUS_NEW

/* :nodoc:
 *
 * This method depends on the availability of `rb_process_status_new`. See:
 * https://github.com/ruby/ruby/pull/15213
 *
 */
VALUE UM_waitid_status(VALUE self, VALUE idtype, VALUE id, VALUE options) {
  struct um *machine = um_get_machine(self);
  return um_waitid_status(machine, NUM2INT(idtype), NUM2INT(id), NUM2INT(options));
}

#endif

/* call-seq:
 *   machine.prep_timeout(interval) -> timeout
 *
 * Prepares an asynchronous timeout instance.
 *
 * @param interval [Integer] timeout interval in seconds
 * @return [UM::AsyncOp] async operation instance
 */
VALUE UM_prep_timeout(VALUE self, VALUE interval) {
  struct um *machine = um_get_machine(self);
  return um_prep_timeout(machine, NUM2DBL(interval));
}

/* call-seq:
 *   machine.ssl_set_bio(ssl) -> machine
 *
 * Sets up the given ssl socket to use the machine for sending and receiving.
 *
 * @param ssl [OpenSSL::SSL::SSLSocket] ssl socket
 * @return [UringMachine] machine
 */
VALUE UM_ssl_set_bio(VALUE self, VALUE ssl) {
  struct um *machine = um_get_machine(self);
  um_ssl_set_bio(machine, ssl);
  return self;
}

/* call-seq:
 *   machine.ssl_read(ssl, buf, maxlen) -> bytes_read
 *
 * Reads from the given SSL socket. This method should be used after first
 * having called `#ssl_set_bio`.
 *
 * @param ssl [OpenSSL::SSL::SSLSocket] ssl socket
 * @param buf [String, IO::Buffer] buffer
 * @param maxlen [Integer] maximum number of bytes to read
 * @return [Integer] number of bytes read
 */
VALUE UM_ssl_read(VALUE self, VALUE ssl, VALUE buf, VALUE maxlen) {
  struct um *machine = um_get_machine(self);
  int ret = um_ssl_read(machine, ssl, buf, NUM2INT(maxlen));
  return INT2NUM(ret);
}

/* call-seq:
 *   machine.ssl_write(ssl, buf, len) -> bytes_written
 *
 * Writes to the given SSL socket. This method should be used after first
 * having called `#ssl_set_bio`.
 *
 * @param ssl [OpenSSL::SSL::SSLSocket] ssl socket
 * @param buf [String, IO::Buffer] buffer
 * @param len [Integer] number of bytes to write
 * @return [Integer] number of bytes written
 */
VALUE UM_ssl_write(VALUE self, VALUE ssl, VALUE buf, VALUE len) {
  struct um *machine = um_get_machine(self);
  int ret = um_ssl_write(machine, ssl, buf, NUM2INT(len));
  return INT2NUM(ret);
}

/* call-seq:
 *   UringMachine.pipe -> [read_fd, write_fd]
 *
 * Creates a pipe, returning the file descriptors for the read and write ends.
 *
 * - https://www.man7.org/linux/man-pages/man2/pipe.2.html
 *
 * @return [Array<Integer>] array containing the read and write file descriptors
 */
VALUE UM_pipe(VALUE self) {
  int fds[2];
  int ret = pipe(fds);
  if (ret) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }

  return rb_ary_new_from_args(2, INT2NUM(fds[0]), INT2NUM(fds[1]));
}

/* call-seq:
 *   UringMachine.socketpair(domain, type, protocol) -> [s1_fd, s2_fd]
 *
 * Creates a pair of connected sockets, returning the two file descriptors.
 *
 * - https://www.man7.org/linux/man-pages/man2/socketpair.2.html
 *
 * @param domain [Integer] domain
 * @param type [Integer] type
 * @param protocol [Integer] protocol
 * @return [Array<Integer>] array containing the two file descriptors
 */
VALUE UM_socketpair(VALUE self, VALUE domain, VALUE type, VALUE protocol) {
  int fds[2];
  int ret = socketpair(NUM2INT(domain), NUM2INT(type), NUM2INT(protocol), fds);
  if (ret) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }

  return rb_ary_new_from_args(2, INT2NUM(fds[0]), INT2NUM(fds[1]));
}

/* call-seq:
 *   UringMachine.pidfd_open(pid) -> fd
 *
 * Creates a file descriptor representing the given process pid. The file
 * descriptor can then be used with methods such as `#waitid` or `.pidfd_open`.
 *
 * - https://www.man7.org/linux/man-pages/man2/pidfd_open.2.html
 *
 * @param pid [Integer] process pid
 * @return [Integer] file descriptor
 */
VALUE UM_pidfd_open(VALUE self, VALUE pid) {
  int fd = syscall(SYS_pidfd_open, NUM2INT(pid), 0);
  if (fd == -1) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }

  return INT2NUM(fd);
}

/* call-seq:
 *   UringMachine.pidfd_send_signal(fd, sig) -> fd
 *
 * Sends a signal to a pidfd.
 *
 * - https://www.man7.org/linux/man-pages/man2/pidfd_send_signal.2.html
 *
 * @param fd [Integer] pidfd
 * @param sig [Integer] signal
 * @return [Integer] pidfd
 */
VALUE UM_pidfd_send_signal(VALUE self, VALUE fd, VALUE sig) {
  int ret = syscall(
    SYS_pidfd_send_signal, NUM2INT(fd), NUM2INT(sig), NULL, 0
  );
  if (ret) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }

  return fd;
}

/* call-seq:
 *   UringMachine.kernel_version -> version
 *
 * Returns the kernel version.
 *
 * @return [Integer] kernel version
 */
VALUE UM_kernel_version(VALUE self) {
  return INT2NUM(UM_KERNEL_VERSION);
}

/* call-seq:
 *   UringMachine.debug(str)
 *
 * Prints the given string to STDERR.
 *
 * @param str [String] debug message
 * @return [void]
 */
VALUE UM_debug(VALUE self, VALUE str) {
  fprintf(stderr, "%s\n", StringValueCStr(str));
  return Qnil;
}

/* call-seq:
 *   UringMachine.inotify_init -> fd
 *
 * Creates an inotify file descriptor.
 *
 * - https://www.man7.org/linux/man-pages/man2/inotify_init.2.html
 *
 * @return [Integer] file descriptor
 */
VALUE UM_inotify_init(VALUE self) {
  int fd = inotify_init();
  if (fd == -1) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }
  return INT2NUM(fd);
}

/* call-seq:
 *   UringMachine.inotify_add_watch(fd, path, mask) -> wd
 *
 * Adds a watch on the given inotify file descriptor.
 *
 * - https://www.man7.org/linux/man-pages/man2/inotify_add_watch.2.html
 *
 * @param fd [Integer] inotify file descriptor
 * @param path [String] file/directory path
 * @param mask [Integer] inotify event mask
 * @return [Integer] watch descriptor
 */
VALUE UM_inotify_add_watch(VALUE self, VALUE fd, VALUE path, VALUE mask) {
  int ret = inotify_add_watch(NUM2INT(fd), StringValueCStr(path), NUM2UINT(mask));
  if (ret == -1) {
    int e = errno;
    rb_syserr_fail(e, strerror(e));
  }
  return INT2NUM(ret);
}

static inline VALUE inotify_get_events(char *buf, size_t len) {
  VALUE array = rb_ary_new();
  while (len > 0) {
    struct inotify_event *evt = (struct inotify_event *)buf;
    size_t evt_len = sizeof(struct inotify_event) + evt->len;

    VALUE hash = rb_hash_new();
    rb_hash_aset(hash, SYM_wd,    INT2NUM(evt->wd));
    rb_hash_aset(hash, SYM_mask,  UINT2NUM(evt->mask));
    rb_hash_aset(hash, SYM_name,  rb_str_new_cstr(evt->name));
    rb_ary_push(array, hash);
    RB_GC_GUARD(hash);

    buf += evt_len;
    len -= evt_len;
  }
  RB_GC_GUARD(array);
  return array;
}

/* call-seq:
 *   machine.inotify_get_events(fd) -> events
 *
 * Waits for and returns one or more events on the given inotify file
 * descriptor. Each event is returned as a hash containing the watch descriptor,
 * the file name, and the event mask.
 *
 * - https://www.man7.org/linux/man-pages/man7/inotify.7.html
 *
 * @param fd [Integer] inotify file descriptor
 * @return [Array<Hash>] array of one or more events
 */
VALUE UM_inotify_get_events(VALUE self, VALUE fd) {
  struct um *machine = um_get_machine(self);

  char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));

  size_t ret = um_read_raw(machine, NUM2INT(fd), buf, sizeof(buf));
  return inotify_get_events(buf, ret);
}

void Init_UM(void) {
  rb_ext_ractor_safe(true);

  cUM = rb_define_class("UringMachine", rb_cObject);
  rb_define_alloc_func(cUM, UM_allocate);

  rb_define_method(cUM, "initialize", UM_initialize, -1);
  rb_define_method(cUM, "size", UM_size, 0);
  rb_define_method(cUM, "mark", UM_mark_m, 1);
  rb_define_method(cUM, "metrics", UM_metrics, 0);
  rb_define_method(cUM, "sqpoll_mode?", UM_sqpoll_mode_p, 0);
  rb_define_method(cUM, "profile_mode?", UM_profile_mode_p, 0);
  rb_define_method(cUM, "profile_mode=", UM_profile_mode_set, 1);
  rb_define_method(cUM, "test_mode=", UM_test_mode_set, 1);

  rb_define_method(cUM, "sidecar_mode?", UM_sidecar_mode_p, 0);
  rb_define_method(cUM, "sidecar_start", UM_sidecar_start, 0);
  rb_define_method(cUM, "sidecar_stop", UM_sidecar_stop, 0);

  rb_define_method(cUM, "setup_buffer_ring", UM_setup_buffer_ring, 2);

  rb_define_singleton_method(cUM, "pipe", UM_pipe, 0);
  rb_define_singleton_method(cUM, "socketpair", UM_socketpair, 3);
  rb_define_singleton_method(cUM, "pidfd_open", UM_pidfd_open, 1);
  rb_define_singleton_method(cUM, "pidfd_send_signal", UM_pidfd_send_signal, 2);

  rb_define_singleton_method(cUM, "kernel_version", UM_kernel_version, 0);
  rb_define_singleton_method(cUM, "debug", UM_debug, 1);

  rb_define_singleton_method(cUM, "inotify_init", UM_inotify_init, 0);
  rb_define_singleton_method(cUM, "inotify_add_watch", UM_inotify_add_watch, 3);

  rb_define_method(cUM, "schedule", UM_schedule, 2);
  rb_define_method(cUM, "snooze", UM_snooze, 0);
  rb_define_method(cUM, "timeout", UM_timeout, 2);
  rb_define_method(cUM, "yield", UM_yield, 0);
  rb_define_method(cUM, "switch", UM_switch, 0);
  rb_define_method(cUM, "wakeup", UM_wakeup, 0);
  rb_define_method(cUM, "submit", UM_submit, 0);
  rb_define_method(cUM, "pending_fibers", UM_pending_fibers, 0);

  rb_define_method(cUM, "close", UM_close, 1);
  rb_define_method(cUM, "close_async", UM_close_async, 1);
  rb_define_method(cUM, "open", UM_open, 2);
  rb_define_method(cUM, "read", UM_read, -1);
  rb_define_method(cUM, "read_each", UM_read_each, 2);
  rb_define_method(cUM, "sleep", UM_sleep, 1);
  rb_define_method(cUM, "periodically", UM_periodically, 1);
  rb_define_method(cUM, "write", UM_write, -1);
  rb_define_method(cUM, "writev", UM_writev, -1);
  rb_define_method(cUM, "write_async", UM_write_async, -1);
  rb_define_method(cUM, "statx", UM_statx, 4);

  rb_define_method(cUM, "poll", UM_poll, 2);
  rb_define_method(cUM, "select", UM_select, 3);
  rb_define_method(cUM, "waitid", UM_waitid, 3);

  #ifdef HAVE_RB_PROCESS_STATUS_NEW
  rb_define_method(cUM, "waitid_status", UM_waitid_status, 3);
  #endif

  rb_define_method(cUM, "accept", UM_accept, 1);
  rb_define_method(cUM, "accept_each", UM_accept_each, 1);
  rb_define_method(cUM, "accept_into_queue", UM_accept_into_queue, 2);
  rb_define_method(cUM, "bind", UM_bind, 3);
  rb_define_method(cUM, "connect", UM_connect, 3);
  rb_define_method(cUM, "getsockopt", UM_getsockopt, 3);
  rb_define_method(cUM, "listen", UM_listen, 2);
  rb_define_method(cUM, "recv", UM_recv, 4);
  rb_define_method(cUM, "recv_each", UM_recv_each, 3);
  rb_define_method(cUM, "send", UM_send, 4);

  #ifdef HAVE_IO_URING_SEND_VECTORIZED
  rb_define_method(cUM, "sendv", UM_sendv, -1);
  #endif

  rb_define_method(cUM, "send_bundle", UM_send_bundle, -1);
  rb_define_method(cUM, "setsockopt", UM_setsockopt, 4);
  rb_define_method(cUM, "socket", UM_socket, 4);
  rb_define_method(cUM, "shutdown", UM_shutdown, 2);
  rb_define_method(cUM, "shutdown_async", UM_shutdown_async, 2);
  rb_define_method(cUM, "send_fd", UM_send_fd, 2);
  rb_define_method(cUM, "recv_fd", UM_recv_fd, 1);

  rb_define_method(cUM, "prep_timeout", UM_prep_timeout, 1);

  rb_define_method(cUM, "pop", UM_queue_pop, 1);
  rb_define_method(cUM, "push", UM_queue_push, 2);
  rb_define_method(cUM, "shift", UM_queue_shift, 1);
  rb_define_method(cUM, "synchronize", UM_mutex_synchronize, 1);
  rb_define_method(cUM, "unshift", UM_queue_unshift, 2);

  rb_define_method(cUM, "ssl_set_bio", UM_ssl_set_bio, 1);
  rb_define_method(cUM, "ssl_read", UM_ssl_read, 3);
  rb_define_method(cUM, "ssl_write", UM_ssl_write, 3);

  rb_define_method(cUM, "inotify_get_events", UM_inotify_get_events, 1);

  eUMError = rb_define_class_under(cUM, "Error", rb_eStandardError);

  um_define_net_constants(cUM);

  SYM_size =            ID2SYM(rb_intern("size"));
  SYM_total_ops =       ID2SYM(rb_intern("total_ops"));
  SYM_total_switches =  ID2SYM(rb_intern("total_switches"));
  SYM_total_waits =     ID2SYM(rb_intern("total_waits"));
  SYM_ops_pending =     ID2SYM(rb_intern("ops_pending"));
  SYM_ops_unsubmitted = ID2SYM(rb_intern("ops_unsubmitted"));
  SYM_ops_runqueue =    ID2SYM(rb_intern("ops_runqueue"));
  SYM_ops_free =        ID2SYM(rb_intern("ops_free"));
  SYM_ops_transient =   ID2SYM(rb_intern("ops_transient"));
  SYM_time_total_cpu =  ID2SYM(rb_intern("time_total_cpu"));
  SYM_time_total_wait = ID2SYM(rb_intern("time_total_wait"));

  SYM_wd              = ID2SYM(rb_intern("wd"));
  SYM_mask            = ID2SYM(rb_intern("mask"));
  SYM_name            = ID2SYM(rb_intern("name"));

  id_fileno = rb_intern_const("fileno");
}
