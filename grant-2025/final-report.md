# Final Report for Ruby Association Grant Program 2025

## Project Summary

Io_uring is a relatively new Linux API, permitting the asynchronous invocation
of Linux system calls. UringMachine is a gem that brings low-level access to the
io_uring interface to Ruby programs, and permits not only asynchronous I/O on
files and sockets, but also timeouts, futex wait/wake, statx and other system
calls, for use with fiber-based concurrency. This project aims to enhance
UringMachine to include a fiber scheduler implementation for usage with the
standard Ruby I/O classes, to have builtin support for SSL, to support more
io_uring ops such as writev, splice, fsync, mkdir, fadvise, etc.

I'd like to express my deep gratitude to Samuel Williams for his help and
guidance throughout this project.

## About UringMachine

UringMachine provides an API that closely follows the shape of various Linux
system calls for I/O operations, such as `open`, `read`, `accept`, `setsockopt`
etc. Behind the scenes, UringMachine uses io_uring to submit I/O operations and
automatically switch between fibers when waiting for those operations to
complete. Here's a simple example of an echo server using UringMachine:

```ruby
require 'uringmachine'

machine = UM.new
server_fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
machine.bind(server_fd, '0.0.0.0', 1234)
machine.listen(server_fd, UM::SOMAXCONN)

def handle_connection(machine, fd)
  buf = +''
  while true
    res = machine.recv(fd, buf, 8192, 0)
    break if res == 0

    machine.send(fd, buf, res, 0)
  end
ensure
  machine.close(fd)
end

while true
  fd = machine.accept(server_fd)
  machine.spin(fd) {
    handle_connection(machine, it)
  }
end
```

UringMachine also provides some higher-level methods and abstractions that make
use of more advanced io_uring features such as [multishot
operations](https://man.archlinux.org/man/io_uring_multishot.7.en) (e.g.
`UM#accept_each`) and [provided buffer
rings](https://man.archlinux.org/man/io_uring_provided_buffers.7.en) (the
`UM::Stream` class). This not only results in more concise code, but also has
[performance](#benchmarking) benefits. In addition, UringMachine provides some
convenience methods, such as `tcp_listen` which returns a TCP socket fd bound to
the given address and ready for accepting incoming connections.

Here's the same echo server program but using UringMachine's more advanced
features:

```ruby
require 'uringmachine'

machine = UM.new
# Combined socket, bind, listen
server_fd = machine.tcp_listen('0.0.0.0', 1234)

def handle_connection(machine, fd)
  stream = machine.stream(fd)
  stream.each { |buf| machine.send(fd, buf, buf.size, 0) }
ensure
  machine.close(fd)
end

machine.accept_each(server_fd) do |fd|
  machine.spin(fd) {
    handle_connection(machine, it)
  }
end
```

UringMachine also includes a fiber scheduler implementation that effectively
provides an alternative backend for performing I/O operations while still using
the same familiar API of `IO`, `Socket` and related standard classes:

```ruby
machine = UM.new
scheduler = UM::FiberScheduler.new(machine)
Fiber.set_scheduler(scheduler)

Fiber.schedule {
  puts('Hello from UringMachine!')
}

# await all pending fibers
scheduler.join
```

## The Work Done

### Improvements to the Ruby `Fiber::Scheduler` interface

- [PR](https://github.com/ruby/ruby/pull/15213) to expose
  `rb_process_status_new` internal Ruby C API
  (https://bugs.ruby-lang.org/issues/21704). This is needed in order to allow
  FiberScheduler implementations to instantiate `Process::Status` objects in the
  `#process_wait` hook. This PR is still not merged.

- [PR](https://github.com/ruby/ruby/pull/15385) to cleanup FiberScheduler and
  fiber state in a forked process (https://bugs.ruby-lang.org/issues/21717).
  This was merged into Ruby 4.0.

- [PR](https://github.com/ruby/ruby/pull/15609) to invoke FiberScheduler
  `io_write` hook on IO flush (https://bugs.ruby-lang.org/issues/21789). This
  was merged into Ruby 4.0.

- Found an issue while implementing the `#io_pwrite` hook, which resulted in a
  [PR](https://github.com/ruby/ruby/pull/15428) submitted by Samuel Williams,
  and merged into Ruby 4.0.

- Worked with Samuel Williams on how to implement the `#io_close` hook, which
  resulted in a [PR](https://github.com/ruby/ruby/pull/15434) submitted by
  Samuel and merged into Ruby 4.0.

- [PR](https://github.com/ruby/ruby/pull/15865) to add socket I/O hooks to the
  FiberScheduler interface (https://bugs.ruby-lang.org/issues/21837). This PR is
  currently in draft phase, waiting input from Samuel Williams.

### UringMachine `Fiber::Scheduler` Implementation

- I developed a [full
  implementation](https://github.com/digital-fabric/uringmachine/blob/main/lib/uringmachine/fiber_scheduler.rb)
  of the `Fiber::Scheduler` interface using UringMachine, with methods for all
  hooks:

  - `#scheduler_close`
  - `#fiber`, `#yield`
  - `#blocking_operation_wait`, `#block`, `#unblock`, `#fiber_interrupt`
  - `#kernel_sleep`, `#timeout_after`
  - `#io_read`, `#io_write`, `#io_pread`, `#io_pwrite`, `#io_close`
  - `#io_wait`, `#io_select`
  - `#process_wait` (relies on the `rb_process_status_new` PR)
  - `#address_resolve`

- Wrote [extensive
  tests](https://github.com/digital-fabric/uringmachine/blob/main/test/test_fiber_scheduler.rb)
  for the UringMachine fiber scheduler.

### SSL Integration

- I've added UringMachine method that installs a [custom OpenSSL
  BIO](https://github.com/digital-fabric/uringmachine/blob/main/ext/um/um_ssl.c)
  on the given SSLSocket, as well as read/write methods that provide higher
  throughput on SSL connections:

  ```ruby
  ssl = OpenSSL::SSL::SSLSocket.new(sock, OpenSSL::SSL::SSLContext.new)
  machine.ssl_set_bio(ssl)
  msg = 'foo'
  ssl.write(msg) # Performs IO using the UringMachine BIO

  # Or: bypass the OpenSSL gem, directly invoking SSL_write and using the
  # UringMachine BIO
  machine.ssl_write(ssl, msg, msg.bytesize)
  ```

- I've also submitted a [PR to the OpenSSL
  gem](https://github.com/ruby/openssl/pull/1000) that adds support for using a
  custom BIO method for reading and writing on SSL connections.

### Buffered Reads Using io_uring Provided Buffers

- I developed the `Stream` API that uses io_uring [provided
  buffers](https://man.archlinux.org/man/io_uring_provided_buffers.7.en) to
  allow buffered reads (e.g. similar behavior to `IO#gets`, `IO#readpartial`)
  while minimizing copying of data.
- The [buffer
  pool](https://github.com/digital-fabric/uringmachine/blob/main/ext/um/um_buffer_pool.c):
  UringMachine allocates and provides buffers for kernel usage in multishot
  read/recv operations. An adaptive algorithm ensures that the kernel always has
  enough buffer space for reading data. When a buffer is no longer used by the
  kernel or the stream, it is recycled and eventually provided back to the
  kernel.
- Streams are
  [implemented](https://github.com/digital-fabric/uringmachine/blob/main/ext/um/um_stream.c)
  as a linked list of buffer segments, directly referencing the buffers shared
  with the kernel and referenced in CQEs.
- Stream read methods suitable for both line-based protocols and binary
  protocols.
- An `:ssl` stream mode allows reading from a `SSLSocket` instead of from an fd,
  using the custom SSL BIO discussed above.

### Other Improvements to UringMachine

- Improved various internal aspects of the C-extension: performance and
  correctness of mutex and queue implementations.

- Added support for accepting instances of `IO::Buffer` as buffer for the
  various I/O operations, in order to facilitate the `Fiber::Scheduler`
  implementation.

- Added various methods for working with processes:

  - `UringMachine#waitid`
  - `UringMachine.pidfd_open`
  - `UringMachine.pidfd_send_signal`

- Added detailed internal metrics.

- Added support for vectorized write/send using io_uring: `UringMachine#writev`
  and `UringMachine#sendv`.

- Added support for `SQPOLL` mode - this io_uring mode lets us avoid entering
  the kernel when submitting I/O operations as the kernel is busy polling the SQ
  ring.

- Added support for sidecar mode: an auxiliary thread is used to enter the
  kernel and wait for CQE's (I/O operation completion entries), letting the Ruby
  thread avoid entering the kernel in order to wait for CQEs.

- Added support for watching file system events using the `inotify` interface.
- Methods for low-level work with `inotify`:

  ```ruby
  fd = UM.inotify_init
  wd = UM.inotify_add_watch(fd, '/tmp', UM::IN_CLOSE_WRITE)
  IO.write('/tmp/foo.txt', 'foofoo')
  events = machine.inotify_get_events(fd)
  #=> { wd: wd, mask: UM::IN_CLOSE_WRITE, name: 'foo.txt' }
  ```

- A higher-level API for watching directories:

  ```ruby
  mask = UM::IN_CREATE | UM::IN_DELETE | UM::IN_CLOSE_WRITE
  machine.file_watch(FileUtils.pwd, mask) { handle_fs_event(it) }
  ```

- Added more low-level methods for performing I/O operations supported by
  io_uring: `splice`, `tee`, `fsync`.

### Benchmarking

- I did extensive benchmarking comparing different solutions for performing
  concurrent I/O in Ruby:

  - Using normal Ruby threads
  - Using Samuel's [Async](https://github.com/socketry/async/) gem which
    implements a `Fiber::Scheduler`
  - Using the UringMachine `Fiber::Scheduler`
  - Using the UringMachine low-level API

- The benchmarks simulate different kinds of workloads:

  - Writing and reading from pipes
  - Writing and reading from sockets
  - Doing CPU-bound work synchronized by mutex
  - Doing I/O-bound work synchronized by mutex
  - Pushing and pulling items from queues
  - Running queries on a PostgreSQL database

- The results are here: https://github.com/digital-fabric/uringmachine/blob/main/benchmark/README.md
