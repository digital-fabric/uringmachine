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
  still need to figure out what is going on there and why it is so complex. FWIW, UringMachine has a `#close_async` method which, as its name suggests, submits a close operation, but does not wait for it to complete.

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
