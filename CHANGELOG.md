# 0.24.0 2026-01-30

- Add `Stream.resp_encode_cmd`
- Add sidecar mode
- Add test mode, remove special handling of OP_SCHEDULE in um_switch, do it only
  in test mode
- Improve fiber scheduler error handling, add tests for I/O errors

# 0.23.1 2025-12-16

- Add `MSG_NOSIGNAL` to default flags for `#sendv` and `#send_bundle`

# 0.23.0 2025-12-16

- Add `UM#accept_into_queue`, fix `#accept_each` to throw on error
- Use Set instead of Hash for holding pending fibers
- Add `UM#writev`, `UM#sendv` methods
- Allocate um_op and um_op_result in batches of 256
- Remove `SIGCLD` const

# 0.22.1 2025-12-11

- Comment out SIGCLD constant

# 0.22.0 2025-12-10

- Fix use of `um_yield` in statx, multishot ops
- Improve performance of `UM#snooze`
- Add some profiling info (WIP)
- Add `UM#metrics` for getting metrics
- Add `UM#pending_fibers` for detecting leaking fibers in tests
- More tests and benchmarks
- Add `UM#await_fibers` for awaiting fibers
- Add `UM.socketpair` for creating a socket pair
- Fix segfault caused by waiting fibers not being marked
- Fiber scheduler:
  - Use fiber's mailbox for processing blocking operations
  - Add `#io_close`, `#yield` hooks, remove `#process_fork` hook

# 0.21.0 2025-12-06

- Add `UM#submit`
- Update liburing
- Do not release GVL in um_submit if SQ does not need entering the kernel
- Fix compilation when rb_process_status_new is not available
- Fix um_futex_wake_transient to submit SQE, fix futex_wait usage
- Add debug logging for key io_uring interactions
- Add UM#mark and DEBUG_MARK for debugging specific UM instances
- Short-circuit zero-length writes
- Add optional file_offset argument to #read, #write. Add optional len and
  file_offset arguments to #write_async
- Add support for specifying SQPOLL mode and SQ idle timeout in `UM#initialize`
- Add support for specifying number of SQ entries in `UM#initialize`
- Implement global worker pool for blocking operations in fiber scheduler
- Finish implementing all fiber scheduler hooks
- Add `UM#select`
- Add `UM#wakeup`
- Add `UM#total_op_count`

# 0.20.0 2025-11-26

- Add `UM.pidfd_open`, `UM.pidfd_send_signal` methods
- Add `#waitid`, `#waitid_status` methods, remove `#waitpid`
- Set minimal kernel version to 6.7
- Add `UM::Error` exception class
- Add support for `IO::Buffer` in all I/O methods
- Fix and improve mutex and queue implementations
- Add UM.debug method
- Implement Fiber scheduler (WIP)

# 0.19.1 2025-11-03

- Add `RB_GC_GUARD` in `process_runqueue_op`

# 0.19 2025-10-27

- Fix usage of `RAISE_IF_EXCEPTION` after `RB_GC_GUARD`

# 0.18 2025-08-30

- Fix `#write_async` to properly mark the given string

# 0.17 2025-07-15

- Add `#send_bundle` method

# 2025-07-06 Version 0.16

- Load UM version on require

# 2025-06-24 Version 0.15

- Add support for sleeping forever when sleep duration is lte 0

# 2025-06-09 Version 0.14

- Add `#close_async`, `#shutdown_async`

# 2025-06-07 Version 0.13

- Add `#write_async`

# 2025-06-07 Version 0.12.1

- Improve portability of `UM` constants

# 2025-06-03 Version 0.12

- Add buffer, maxlen params to `Stream#get_line`
- Add buffer param to `Stream#get_string`
- Remove `Stream#resp_get_line`, `Stream#resp_get_string` methods

# 2025-06-02 Version 0.11.1

- Fix `UM::Stream` behaviour on GC

# 2025-06-02 Version 0.11

- Implement `UM::Stream` class for read streams

# 2025-05-05 Version 0.10

- Add `Thread#machine`
- Add `Fiber#mailbox`

# 2025-05-05 Version 0.9

- Add `#statx`

# 2025-05-05 Version 0.8.2

- Correctly deal with -EAGAIN when waiting for CQEs

# 2025-05-05 Version 0.8.1

- Fix `#snooze` to not block waiting for CQE

# 2025-05-03 Version 0.8

- Add `#join`

# 2025-04-28 Version 0.7

- Add `#shutdown`

# 2025-04-23 Version 0.6.1

- Improve `#snooze` to prevent op completion starvation

# 2025-04-23 Version 0.6

- Add `#periodically` for multishot timeout
- Add `UM::Actor` class
- Add `#prep_timeout` and `AsyncOp`

# 2024-11-14 Version 0.5

- Add `#waitpid`
- Add `UM.pipe`, `UM.kernel_version`
- Add `#open`
- Fix `#spin`
- Fix handling of signal interruption.
- Reimplement and simplify um_op
- Add `UM::Queue`, `#push`, `#pop`, `#shift`, `#unshift`
- Add `UM::Mutex`, `#synchronize`
- Add `#recv_each`
- Add `#getsockopt`, `#setsockopt`
- Simplify and improve op management

# 2024-10-06 Version 0.4

- Add socket constants
- Add `#bind`, `#listen`
- Add `#spin`
- Fix bugs in multishot read and other ops
- Add `#recv`, `#send`
- Add `#socket`, `#connect`

# 2024-10-04 Version 0.3

- Fix race condition affecting `#timeout` and `#sleep`.
- Add `#accept_each`
- Add `#accept`

# 2024-10-03 Version 0.2

- Remove old IOU code.
- Add `#read_each`
- Add `#read`

# 2024-10-03 Version 0.1

The basic fiber scheduling stuff

- `#schedule`, `#interrupt`
- `#snooze`, `#yield`
- `#timeout`, `#sleep`
