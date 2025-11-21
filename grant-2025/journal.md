## 2025-11-14

### Call with Samuel

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

