# Interim Report for Ruby Association Grant Program 2025

## Project Summary 

Io_uring is a relatively new Linux API, permitting the invocation of Linux
system calls asynchronously. UringMachine is a gem that brings low-level access
to the io_uring interface to Ruby programs, and permits not only asynchronous
I/O on files and sockets, but also timeouts, futex wait/wake, statx etc, with
support for fiber-based concurrency. This project will work to enhance
UringMachine to include a fiber scheduler implementation for usage with the
standard Ruby I/O classes, to have builtin support for SSL, to support more
io_uring ops such as writev, splice, fsync, mkdir, fadvise, etc.

## Progress Report

As of the present date, I have worked on the following:

### Improvements to the Ruby `Fiber::Scheduler` interface

- [PR](https://github.com/ruby/ruby/pull/15213) to expose
  `rb_process_status_new` internal Ruby C API
  (https://bugs.ruby-lang.org/issues/21704). This is needed in order to allow
  FiberScheduler implementations to instantiate `Process::Status` objects in the
  `#process_wait` hook. This PR is still pending a decision by the Ruby core team.

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
  currently in draft phase.

### UringMachine `Fiber::Scheduler` Implementation

- I developed a [full
  implementation](https://github.com/digital-fabric/uringmachine/blob/main/lib/uringmachine/fiber_scheduler.rb)
  of the `Fiber::Scheduler` interface using UringMachine, with methods for *all*
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

### Improvements to UringMachine

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
  and `UringMachine#sendv`

- Added support for `SQPOLL` mode - this io_uring mode lets us avoid entering
  the kernel when submitting I/O operations as the kernel is busy polling the SQ
  ring.

- Added support for sidecar mode: an auxiliary thread is used to enter the
  kernel and wait for CQE's (I/O operation completion entries), letting the Ruby
  thread avoid entering the kernel in order to wait for CQEs.

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

### Pending Work

Before the end of the grant work period I intend to do the following:

- I already started work on SSL integration. I intend to contribute changes to
  `ruby/openssl` to add support for custom BIO that will use the underlying
  socket for performing I/O (currently the Ruby openssl implementation
  completely bypasses the Ruby I/O layer in order to send/recv to sockets). This
  will allow integration with the `Fiber::Scheduler` interface.

- Add support for automatic buffer management for performing mutlishot read/recv
  using io_uring's registered buffers feature.

- Add some more low-level methods for performing I/O operations supported by
  io_uring: splice, fsync, mkdir, fadvise etc.