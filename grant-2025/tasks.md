- [v] io-event

  - [v] Make PR to use io_uring_prep_waitid for kernel version >= 6.7

    - https://github.com/socketry/io-event/blob/44666dc92ac3e093ca6ce3ab47052b808a58a325/ext/io/event/selector/uring.c#L460
    - https://github.com/digital-fabric/uringmachine/blob/d5505d7fd94b800c848d186e17585e03ad9af6f2/ext/um/um.c#L697-L713

- [ ] UringMachine
  - [v] Add support for IO::Buffer in UM API. (How can we detect an IO::Buffer object?)
        https://docs.ruby-lang.org/capi/en/master/d8/d36/group__object.html#gab1b70414d07e7de585f47ee50a64a86c

  - [v] Add `UM::Error` class to be used instead of RuntimeError

  - [ ] Do batch allocation for `struct um_op`, so they'll be adjacent
  - [ ] Add optional buffer depth argument to `UM.new` (for example, a the
    worker thread for the scheduler `blocking_operation_wait` hook does not need
    a lot of depth, so you can basically do `UM.new(4)`)

  - [ ] Add support for using IO::Buffer in association with io_uring registered buffers / buffer rings

- [ ] UringMachine Fiber::Scheduler implementation
4
  - [v] Check how scheduler interacts with `fork`.
  - [v] Implement `process_wait` (with `rb_process_status_new`)
  - [v] Add tests:
    - [v] Sockets (only io_wait)
    - [v] Files
    - [v] Mutex / Queue
    - [v] Thread.join
    - [v] Process.wait
    - [v] fork
    - [v] system / exec / etc.
    - [v] popen
  - [ ] Implement timeouts (how do timeouts interact with blocking ops?)
    - [ ] Add `#timeout_after` hook
          https://github.com/socketry/async/blob/ea8b0725042b63667ea781d4d011786ca3658256/lib/async/scheduler.rb#L631-L644
    - [ ] Add timeout handling in different I/O hooks
    - [ ] Add tests
  - [ ] Add `#address_resolve` hook with same impl as Async:
        https://github.com/socketry/async/blob/ea8b0725042b63667ea781d4d011786ca3658256/lib/async/scheduler.rb#L285-L296

  - [ ] Benchmarks
    - [ ] UM queue / Ruby queue (threads) / Ruby queue with UM fiber scheduler
    - [ ] UM mutex / Ruby mutex (threads) / Ruby mutex with UM fiber scheduler
    - [ ] Pipe IO raw UM / Ruby threaded / Ruby with UM fiber scheduler
    - [ ] Socket IO (with socketpair) raw UM / Ruby threaded / Ruby with UM fiber scheduler
    - [ ] Measure CPU (thread) time usage for above examples

        - run each version 1M times
        - measure total real time, total CPU time

        ```ruby
        real_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        cpu_time = Process.clock_gettime(Process::CLOCK_THREAD_CPUTIME_ID)
        ```

        - my hunch is we'll be able to show with io_uring real_time is less,
          while cpu_time is more. But it's just a hunch.


- [ ] Ruby Fiber::Scheduler interface
  - [ ] Missing hooks for send/recv/sendmsg/recvmsg
  - [ ] Writes to a file (including `IO.write`) do not invoke `#io_write` (because writes to files cannot be non-blocking.) Instead, `blocking_operation_wait` is invoked.


- [ ] SSL
  - [ ] openssl gem: custom BIO?

    - curl: https://github.com/curl/curl/blob/5f4cd4c689c822ce957bb415076f0c78e5f474b5/lib/vtls/openssl.c#L786-L803

