# 2025-11-14

## Call with Samuel

- I explained the tasks that I want to do:

  1. FiberScheduler implementation for UringMachine
  2. Async SSL I/O
  3. Extend UringMachine & FiberScheduler with new functionality

- Samuel talked about two aspects:

  - Experimentation.
  - Integrating and improving on existing ecosystem, publicly visible changes to
    interfaces.

So, improve on FiberScheduler interface, and show UringMachine as implementation.

Suggestion for tasks around FiberScheduler from Samuel:

1. Add Fiber::Scheduler#io_splice + IO-uring backing for IO.copy_stream

Summary:

Build an async-aware, zero-copy data-transfer path in Ruby by exposing Linux’s
splice(2) through the Fiber Scheduler, and wiring it up so IO.copy_stream can
take advantage of io_uring when available. Why it matters: Large file copies and
proxying workloads become dramatically faster and cheaper because the data never
touches user space. This gives Ruby a modern, high-performance primitive for
bulk I/O.

2. Add support for registered IO-uring buffers via IO::Buffer

Summary:

Integrate io_uring’s “registered buffers” feature with Ruby’s IO::Buffer,
allowing pre-allocated, pinned buffers to be reused across operations.

Why it matters:

Drastically reduces syscalls and buffer management overhead. Enables fully
zero-copy, high-throughput network servers and a more direct path to competitive
I/O performance.

3. Richer process APIs using pidfds (Fiber::Scheduler#process_open)

Summary:

Introduce pidfd-backed process primitives in Ruby so processes can be opened,
monitored, and waited on safely through the scheduler.

Why it matters:

Pidfds eliminate race conditions, improve cross-thread safety, and make process
management reliably asynchronous. This enables safer job-runners, supervisors,
and async orchestration patterns in Ruby.

4. Proper fork support for Fiber Scheduler (Fiber::Scheduler#process_fork)

Summary:

Define how fiber schedulers behave across fork: the child should start in a
clean state, with hooks to reinitialize or discard scheduler data safely.

Why it matters:

fork + async currently work inconsistently. This project makes forking
predictable, allowing libraries and apps to do post-fork setup (e.g., reconnect
I/O, restart loops) correctly and safely.

5. Async-aware IO#close via io_uring prep_close + scheduler hook

Summary:

Introduce a formal closing state in Ruby’s IO internals, add io_uring’s
prep_close support, and provide Fiber::Scheduler#io_close as an official hook.

Why it matters:

Today, IO#close can be slow or unsafe to call in async contexts because it must
run synchronously. This project allows deferred/batched closing, avoids races,
and modernizes Ruby’s internal I/O lifecycle.

GDB/LLDB extensions: https://github.com/socketry/toolbox

# 2025-11-17

## Work on io-event Uring selector

I added an implementation of `process_wait` using `io_uring_prep_waitid`. This
necessitates being able to create instances of `Process::Status`. For this, I've
submitted a PR for exposing `rb_process_status_new`:
https://github.com/ruby/ruby/pull/15213. Hopefully, this PR will be merged
before the release of Ruby 4.0.

# 2025-11-21

## Work on UringMachine Fiber Scheduler

I've finally made some progress on the UringMachine fiber scheduler. This was a
process of learning the mchanics of how the scheduler is integrated with the
Ruby I/O layer. Some interesting warts in the Ruby `IO` implementation:

- When you call `Kernel.puts`, the trailing newline character is actually
  written separately, which can lead to unexpected output if for example you
  have multiple fibers writing to STDOUT at the same time. To prevent this, Ruby
  uses a mutex (per IO instance) to synchronize writes to the same IO.
- There are inconsistencies in how different kinds of IO objects are handled,
  with regards to blocking/non-blocking operation
  ([O_NONBLOCK](https://linux.die.net/man/2/fcntl)):

  - Files and standard I/O are blocking.
  - Pipes are non-blocking.
  - Sockets are non-blocking.
  - OpenSSL sockets are non-blocking.

  The problem is that for io_uring to function properly, the fds passed to it
  should always be in blocking mode. To rectify this, I've added code to the
  fiber scheduler implementation that makes sure the IO instance is blocking:

  ```ruby
  def io_write(io, buffer, length, offset)
    reset_nonblock(io)
    @machine.write(io.fileno, buffer.get_string)
  rescue Errno::EINTR
    retry
  end

  def reset_nonblock(io)
    return if @ios.key?(io)

    @ios[io] = true
    UM.io_set_nonblock(io, false)
  end
  ```

- A phenomenon I've observed is that in some situations of multiple fibers doing
  I/O, some of those I/O operations would raise an `EINTR`, which should mean
  the I/O operation was interrupted because of a signal sent to the process.
  Weird!

- There's some interesting stuff going on when calling `IO#close`. Apparently
  there's a mutex involved, and I noticed two scheduler hooks are being called:
  `#blocking_operation_wait` which means a blocking operation that should be ran
  on a separate thread, and `#block`, which means a mutex is being locked. I
  still need to figure out what is going on there and why it is so complex.
  FWIW, UringMachine has a `#close_async` method which, as its name suggests,
  submits a close operation, but does not wait for it to complete.

- I've added some basic documentation to the `FiberScheduler` class, and started
  writing some tests. Now that I have a working fiber scheduler implementation
  and I'm beginning to understand the mechanics of it, I can start TDD'ing...

## Work on io-event Uring selector

- I've submitted a [PR](https://github.com/socketry/io-event/pull/154) for using
  `io_uring_prep_waitid` in the `process_wait` implementation. This relies on
  having a recent Linux kernel (>=6.7) and the afore-mentioned Ruby
  [PR](https://github.com/ruby/ruby/pull/15213) for exposing
  `rb_process_status_new` being merged. Hopefully this will happen in time for
  the Ruby 4.0 release.

# 2025-11-26

- Added some benchmarks for measuring mutex performance vs stock Ruby Mutex
  class. It turns out the `UM#synchronize` was much slower than core Ruby
  `Mutex#synchronize`. This was because the UM version was always performing a
  futex wake before returning, even if no fiber was waiting to lock the mutex. I
  rectified this by adding a `num_waiters` field to `struct um_mutex`, which
  indicates the number of fibers currently waiting to lock the mutex, and
  avoiding calling `um_futex_wake` if it's 0.

- I also noticed that the `UM::Mutex` and `UM::Queue` classes were marked as
  `RUBY_TYPED_EMBEDDABLE`, which means the underlying `struct um_mutex` and
  `struct um_queue` were subject to moving. Obviously, you cannot just move a
  futex var while the kernel is potentially waiting on it to change. I fixed
  this by removing the `RUBY_TYPED_EMBEDDABLE` flag. This is a possible
  explanation for the occasional segfaults I've been seeing in Syntropy when
  doing lots of cancelled `UM#shift` ops (watching for file changes). (commit 3b013407ff94f8849517b0fca19839d37e046915)

- Added support for `IO::Buffer` in all low-level I/O APIs, which also means the
  fiber scheduler doesn't need to convert from `IO::Buffer` to strings in order
  to invoke the UringMachine API. (commits
  620680d9f80b6b46cb6037a6833d9cde5a861bcd,
  16d2008dd052e9d73df0495c16d11f52bee4fd15,
  4b2634d018fdbc52d63eafe6b0a102c0e409ebca,
  bc9939f25509c0432a3409efd67ff73f0b316c61,
  a9f38d9320baac3eeaf2fcb2143294ab8d115fe9)

- Added a custom `UM::Error` exception class raised on bad arguments or other
  API misuse. I've also added a `UM::Stream::RESPError` exception class to be
  instantiated on RESP errors. (commit 72a597d9f47d36b42977efa0f6ceb2e73a072bdf)

- I explored the fiber scheduler behaviour after forking. A fork done from a
  thread where a scheduler was set will result in a main thread with the same
  scheduler instantance. For the scheduler to work correctly after a fork, its
  state must be reset. This is because sharing the same io_uring instance
  between parent and child processes is not possible
  (https://github.com/axboe/liburing/issues/612), and also because the child
  process keeps only the fiber from which the fork was made as its main fiber
  (the other fibers are lost).

  So, the right thing to do here would be to add a `Fiber::Scheduler` hook that
  will be invoked automatically by Ruby after a fork, and together with Samuel
  I'll see if I can prepare a PR for that to be merged for the Ruby 4.0 release.

  For the time being, I've added a `#post_fork` method to the UM fiber scheduler
  which should be manually called after a fork. (commit
  2c7877385869c6acbdd8354e2b2909cff448651b)

- Added two new low-level APIs for waiting on processes, instead of
  `UM#waitpid`, using the io_uring version of `waitid`. The vanilla version
  `UM#waitid` returns an array containing the terminated process pid, exit
  status and code. The `UM#waitid_status` method returns a `Process::Status`
  with the pid and exit status. This method is present only if the
  `rb_process_status_new` function is available (see above).

- Implemented `FiberScheduler#process_wait` hook using `#waitid_status`.

- For the sake of completeness, I also added `UM.pidfd_open` and
  `UM.pidfd_send_signal` for working with PID. A simple example:

  ```ruby
  child_pid = fork { ... }
  fd = UM.pidfd_open(child_pid)
  ...
  UM.pidfd_send_signal(fd, UM::SIGUSR1)
  ...
  pid2, status = machine.waitid(P_PIDFD, fd, UM::WEXITED)
  ```

# 2025-11-28

- On Samuel's suggestions, I've submitted a
  [PR](https://github.com/ruby/ruby/pull/15342) for adding a
  `Fiber::Scheduler#process_fork` hook that is automatically invoked after a
  fork. This is in continuation to the `#post_fork` method. I still have a lot
  to learn about working with the Ruby core code, but I'm really excited about
  the possibility of this PR (and the [previous
  one](https://github.com/ruby/ruby/pull/15213) as well) getting merged in time
  for the Ruby 4.0 release.
- Added a bunch of tests for `UM::FiberScheduler`: socket I/O, file I/O, mutex,
  queue, waiting for threads. In the process I discovered a lots of things that
  can be improved in the way Ruby invokes the fiber scheduler.

  - For regular files, Ruby assumes file I/O can never be non-blocking (or
    async), and thus invokes the `#blocking_operation_wait` hook in order to
    perform the I/O in a separate thread. With io_uring, of course, file I/O
    *is* asynchronous.
  - For sockets there are no specialized hooks, like `#socket_send` etc.
    Instead, Ruby makes the socket fd's non-blocking and invokes `#io_wait` to
    wait for the socket to be ready.
  
  I find it interesting how io_uring breaks a lot of assumptions about how I/O
  should be done.

# 2025-12-03

- Samuel and me continued discussing the behavior of the fiber scheduler after a
  fork. After talking it through, we decided the best course of action would be
  to remove the fiber scheduler after a fork, rather than to introduce a
  `process_fork` hook. This is a safer choice, since a scheduler risks carrying
  over some of its state across a fork, leading to unexpected behavior.

  Another problem I uncovered is that if a fork is done from a non-blocking
  fiber, the main fiber of the forked process (which "inherits" the forking
  fiber) stays in non-blocking mode, which also may lead to unexpected behavior,
  since the main fiber of all Ruby threads should be in blocking mode.

  So I submitted a new [PR](https://github.com/ruby/ruby/pull/15385) that
  corrects these two problems.

- I mapped the remaining missing hooks in the UringMachine fiber scheduler
  implementation, and made the tests more robust by checking that the different
  scheduler hooks were actually being called.

- Continued implementing the missing fiber scheduler hooks: `#fiber_interrupt`,
  `#address_resolve`, `#timeout_after`. For the most part, they were simple to
  implement. I probably spent most of my time figuring out how to test these,
  rather than implementing them. Most of the hooks involve just a few lines of
  code, with many of them consisting of a single line of code, calling into the
  relevant UringMachine low-level API.

- Implemented the `#io_select` hook, which involved implementing a low-level
  `UM#select` method. This method took some effort to implement, since it needs
  to handle an arbitrary number of file descriptors to check for readiness. We
  need to create a separate SQE for each fd we want to poll. When one or more
  CQEs arrive for polled fd's, we also need to cancel all poll operations that
  have not completed.
  
  Since in many cases, `IO.select` is called with just a single IO, I also added
  a special-case implementation of `UM#select` that specifically handles a
  single fd.

- Implemented a worker pool for performing blocking operations in the scheduler.
  Up until now, each scheduler started their own worker thread for performing
  blocking operations for use in the `#blocking_operation_wait` hook. The new
  implementation uses a worker thread pool shared by all schedulers, with a
  worker count limited to CPU count. Workers are started when needed.

  I also added an optional `entries` argument to set the SQE and CQE buffer
  sizes when starting a new `UringMachine` instance. This is specifically used
  by worker threads to limit the memory footprint of each worker's UringMachine
  instance. The default size is 4096 SQE entries (liburing by default makes the
  CQE buffer size double that of the SQE buffer).




