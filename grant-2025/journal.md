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

4. [v] Proper fork support for Fiber Scheduler (Fiber::Scheduler#process_fork)

Summary:

Define how fiber schedulers behave across fork: the child should start in a
clean state, with hooks to reinitialize or discard scheduler data safely.

Why it matters:

fork + async currently work inconsistently. This project makes forking
predictable, allowing libraries and apps to do post-fork setup (e.g., reconnect
I/O, restart loops) correctly and safely.

5. [v] Async-aware IO#close via io_uring prep_close + scheduler hook

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

# 2025-12-04

- Implemented a worker pool for performing blocking operations in the scheduler.
  Up until now, each scheduler started their own worker thread for performing
  blocking operations for use in the `#blocking_operation_wait` hook. The new
  implementation uses a worker thread pool shared by all schedulers, with a
  worker count limited to CPU count. Workers are started when needed.

  I also added an optional `entries` argument to set the SQE and CQE buffer
  sizes when starting a new `UringMachine` instance. The default size is 4096
  SQE entries (liburing by default makes the CQE buffer size double that of the
  SQE buffer). The blocking operations worker threads specify a value of 4 since
  they only use their UringMachine instance for popping jobs off the job queue
  and pushing the blocking operation result back to the scheduler.

- Added support for `file_offset` argument in `UM#read` and `UM#write` in
  preparation for implementing the `#io_pread` and `#io_pwrite` hooks. The
  `UM#write_async` API, which permits writing to a file descriptor without
  waiting for the operation to complete, got support for specifying `length` and
  `file_offset` arguments as well. In addition, `UM#write` and `UM#write_async`
  got short-circuit logic for writes with a length of 0.

- Added support for specifying buffer offset in `#io_read` and `#io_write`
  hooks.

- Added support for timeout in `#block`, `#io_read` and `#io_write` hooks.

# 2025-12-05

- I found and fixed a problem with how `futex_wake` was done in the low-level
  UringMachine code handling mutexes and queues. This fixed a deadlock in the
  scheduler background worker pool where clients of the pool where not properly
  woken after the submitted operation was done.

- I finished work on the `#io_pread` and `#io_pwrite` hooks. Unfortunately, the
  test for `#io_pwrite` consistently hangs (not on `IO#pwrite` itself, rather on
  closing the file.) With Samuel's help, hopefully we'll find a solution...

- With those two last hooks, the fiber scheduler implementation is now feature
  complete! While I have written test cases for the different fiber scheduler
  hooks, I'd like to add more tests - and especially tests that exercise
  multiple hooks, tests with high concurrency, and integration tests where I
  check how the fiber scheduler plays with Ruby APIs like `Net::HTTP` and the
  `socket` API in general.

# 2025-12-06

- Samuel has found the issue with pwrite (it turns out the the `#io_pwrite` hook
  was being invoked with the GVL released), and [fixed
  it](https://github.com/ruby/ruby/pull/15428). So now `#pwrite` works correctly
  with a fiber scheduler!

- I followed Samuel's suggestion and incorporated some debug logging into the
  extension code interfacing with liburing, in order to facilitate debugging
  when issues are encountered.

- Added support for [SQPOLL
  mode](https://unixism.net/loti/tutorial/sq_poll.html) when setting up a
  UringMachine instance. It's not clear to me what are the performance
  implications of that, but I'll try to make some time to check this against
  [TP2](https://github.com/noteflakes/tp2), a UringMachine-based web server I'm
  currently using in a bunch of projects.

# 2025-12-07

- I started looking at getting `#io_close` to work, and found out that Samuel
  has already done the work, that is the code was already there, but was
  commented out. Samuel explained that it was impossible to get it to work due
  to the complexity of the implementation of `IO#close`, and indeed when I tried
  it myself I saw that in fact it was just not possible the way the IO state is
  managed when an IO is closed. I then had the idea that maybe we could pass the
  underlying fd instead of the IO object itself to the `#io_close` hook. I tried
  it and indeed it worked without any problems. The only issue is that this
  breaks the convention where the different `io_xxx` hooks take an io as their
  first argument. Nevertheless, I suggested this idea to Samuel and gladly he
  accepted when he saw this is the only we can make this hook work. Samuel then
  proceeded to prepare a [PR](https://github.com/ruby/ruby/pull/15434) and merge
  it.

- Added the `#io_close` hook to the UringMachine fiber scheduler, as well as a
  `#yield` hook for dealing with thread interrupts in response to another
  [PR](https://github.com/ruby/ruby/pull/14700) by Samuel. I also added missing
  docs for the different methods in the fiber scheduler.

# 2025-12-08

- Wrote a bunch of benchmarks for different scenarios comparing threads vs fiber
  scheduler vs low-level UM implementation. The
  [results](https://github.com/digital-fabric/uringmachine/blob/main/benchmark/README.md)
  show the promise of UringMachine and of its fiber scheduler. What is great
  about the fiber scheduler interface is that it provides a significant boost to
  I/O-bound scenarios, with almost no change to the source code (basically, you
  just need to replace `Thread.new` with `Fiber.schedule`).

  These results, though preliminary, seem to validate the approach I took with
  UringMachine - implementing a low-level API and tying it to the entire Ruby
  ecosystem by way of the fiber scheduler interface.

- Spent the rest of the day writing lots of tests for the fiber scheduler. I
  tried to cover the entire `IO` API - both class- and instance methods. I also
  wrote some "integration" tests - different scenarios not unlike those in the
  benchmarks, which exercise the different hooks in the fiber scheduler.

- Added some new APIs to help with testing: `UM#await_fibers` is a method for
  waiting for one or more fibers to terminate. Unlike `UM#join`, it doesn't
  return the return values of the given fibers, it just waits for them to
  terminate. Another new API is `UM.socketpair`, which is like
  `Socket.socketpair` except it returns raw fd's.

- Fixed a tricky bug that caused an occasional segmentation fault while running
  benchmarks. Some fibers waiting an operation to complete were garbage
  collected because there was no reference to them anywhere. I fixed this by
  adding a map of pending fibers at the C-extension level and adding and
  removing pending fibers from it automatically. I also added checking for
  leaking fibers at the end of each test, so the UringMachine instance will not
  hold onto fibers that have terminated.

# 2025-12-09

- Added the Async fiber scheduler to the different benchmarks. Also added an
  SQPOLL mode to the benchmarks. Added a PG client benchmark.

- Fixed some small issues in the UM fiber scheduler and in the UM low-level API
  implementation.

# 2025-12-10

- Refactored the benchmarks, abstracting away all the common code into a
  common class that is then used in the different benchmarks. I also added a
  test for ASync with an epoll selector. I'll try to find some time in the
  coming days to update the results in the repo.

- Added and streamlined metrics that indicate the following:

  - The ring size
  - Total number of ops
  - Total number of fiber switches
  - Total number of waits for CQEs
  - Current number of pending ops
  - Current number of unsubmitted ops
  - Current size of runqueue
  - Current number of transient ops
  - Current number of free ops

  I also added some basic time measurements:

  - Total CPU time
  - Total time spent waiting for CQEs

  These are off by default, but can be enabled by calling `UM#profile(true)`.
  I'd like to do a lot more with profiling, like measuring the CPU time spent on
  each fiber, but I'm a bit apprehensive of the performance costs involved, as
  getting the `CLOCK_THREAD_CPUTIME_ID` clock is relatively slow, and then
  managing this for each fiber means getting and setting a couple of instance
  variables, which can *really* slow things down. On top of that, I'm not that
  sure this is really needed.

- I went through some old benchmarks, reorganized them, get rid of some that
  were irrelevant. There were some really interesting ones: a benchmark
  measuring the cost of different ways of accessing an SQLite DB (using
  [Extralite](https://github.com/digital-fabric/extralite/)): normally, using an
  actor interface, or protected by a mutex. I'll try to follow up with a
  benchmark measuring concurrent access to SQLite DBs, similar to the PG one.

  Another interesting benchmark I found was one for resolving DNS addresses
  using Ruby's builtin `Addrinfo` API, the bundled `resolv` gem, and a basic DNS
  resolver included in UringMachine (I totally forgot I made one). Here too, I'd
  like to add a benchmark to measure how these different solutions do in a
  highly concurrent scenario.

- Thanks to one of these old benchmarks I made a change that more than doubled
  the performance of `UM#snooze`. What this method does is it adds the current
  fiber to the end of the runqueue, and yields control to the next fiber in the
  runqueue, or to process available CQE's. This method is useful for testing,
  but also for yielding control periodically when performing CPU-bound work, in
  order to keep the application responsive and improve latency.

# 2025-12-14

- Changed how `struct um_op`s are allocated. This struct is used to represent
  any io_uring operation. It is also used to represent runqueue entries. Now,
  for most I/O operations, this struct is stack-allocated. But when a new fiber
  is scheduled, or when using the `#timeout` or any of the `#xxx_async` methods,
  like `#close_async` or `#write_async`, we need to use a heap-allocated
  `um_op`, because we don't control its lifetime. In order to minimize
  allocations, once a `um_op` is done with (it's been pulled out of the
  runqueue, or its corresponding CQE has been processed), it is put on a
  freelist in order to be reused when needed. Previously, when the freelist was
  empty, UringMachine would just allocate a new one using `malloc`. Now
  UringMachine allocates a array of 256 structs at once and puts all of them on
  the freelist.
- Implemented the vectorized versions of `#write` and `#send`, so now one can
  use `#writev` and `#sendv` to send multiple buffers at once. This could be
  very useful for situations like sending an HTTP response, which is made of a
  headers part and a body part. Also, `#writev` and `#sendv` are guaranteed to
  write/send the entirety of the given buffers, unlike `#write` and `#send`
  which can do partial write/send (for `#send` you can specify the
  `UM::MSG_WAITALL` flag) to guarantee a complete send.
- With the new built-in `Set` class and its new [C
  API](https://github.com/ruby/ruby/pull/13735), I've switched the internal
  `pending_fibers` holding fibers waiting for an operation to complete, from a
  hash to a set.

# 2025-12-15

- Working more with benchmarks, it has occurred to me that with the current
  design of UringMachine, whenever we check for I/O completions (which is also
  the moment when we make I/O submissions to the kernel), we leave some
  performance on the table. This is because when we call `io_uring_submit` or
  `io_uring_wait_cqes`, we make a blocking system call (namely,
  `io_uring_enter`), and correspondigly we release the GVL.

  What this means is that while we're waiting for the system call to return, the
  GVL is available for another Ruby thread to do CPU-bound work. Normally when
  there's a discussion about concurrency in Ruby, there's this dichotomy: it's
  either threads or fibers. But as described above, even when using fibers and
  io_uring for concurrent I/O, we still need to enter the kernel periodically in
  order to submit operations and process completions. So this is an opportunity
  to yield the GVL to a different thread, which can run some Ruby code while the
  first thread is waiting for the system call to return.

  With that in mind, I modified the benchmark code to see what would happen if
  we run two UringMachine instances on two separate threads. The results are
  quite interesting: splitting the work load between two UringMachine instances
  running on separate threads, we get a marked improvement in performance.
  Depending on the benchmark, we get even better performance if we increase the
  thread count to 4.

  But, as we increase the thread count, we eventually hit diminishing returns
  and risk actually having worse performance than with just a single thread. So,
  at least for the workloads I tested (including a very primitive HTTP/1.1
  server), the sweet spot is between 2 and 4 threads.

  One thing I have noticed though, is that while the pure UM version (i.e. using
  the UM low-level API) gets a boost from running on multiple threads, the UM
  fiber scheduler actually can perform worse. This is also the case for the
  Async fiber scheduler, so this might have to do with the fact that the Ruby IO
  class does a lot of work behind the scenes, including locking write mutexes
  and other stuff that's done when the IO is closed. This is still to be
  investigated...

# 2025-12-16

- Added `UM#accept_into_queue`, which accepts incoming socket connections in a
  loop and pushes them to the given queue.

- Improved error handling in the fiber scheduler, and added more tests. There
  are now about 4.2KLoC of test code, with 255 test cases and 780 assertions. And
  that's without all the tests that depend on the
  [`rb_process_new`](https://github.com/ruby/ruby/pull/15213) API, the PR for
  which is currently still not merged.

- Added a test mode to UringMachine that affects runqueue processing, without
  impacting performance under normal conditions.

# 2025-12-17

- I noticed that the fiber scheduler `#io_write` was not being called on
  `IO#flush` or when closing an IO with buffered writes. So any time the IO
  write buffer needs to be flushed, instead of calling the `#io_write` hook, the
  Ruby I/O layer would just run this on a worker thread by calling the
  `#blocking_operation_wait` hook. I've made a
  [PR](https://github.com/ruby/ruby/pull/15609) to fix this.

# 2025-12-18

- Added a [PR](https://github.com/ruby/ruby/pull/15629) to update Ruby NEWS with
  changes to the FiberScheduler interface.

- I did some more verification work on the fiber scheduler implementation. I
  added more tests and improved error handling in read/write hooks.

- Made some small changes to fiber scheduling. I added a test mode which peeks
  at CQEs on each snooze, in order to facilitate testing.

# 2025-12-20

- Did some more work on benchmarks, and added provisory GVL time measurement.

- Implemented sidecar mode - the basic idea is that UringMachine starts an
  auxiliary thread that loops entering the kernel with a call to
  `io_uring_enter` in order to make CQEs available. On return from the system
  call, it signals through a futex that ready CQEs can be processed. 

  On fiber switch, the next fiber to run is shifted from the runqueue. If the
  runqueue is empty, the UringMachine will wait for the signal, and then process
  all CQEs. The idea is that in a single threaded environment, under high enough
  I/O load,  we don't need to release the GVL in order to process ready CQEs,
  and thus we can better saturate the CPU.

# 2025-12-26

- Finished up the sidecar mode implementation. I did some preliminary benchmarks and this mode does provide a small performance benefit, depending on the context. But for the moment, I consider this mode experimental.

