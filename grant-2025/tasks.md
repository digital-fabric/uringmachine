- [v] io-event
  - [v] Make PR to use io_uring_prep_waitid for kernel version >= 6.7

- [ ] UringMachine low-level API
  - [v] Add support for IO::Buffer in UM API.
  - [v] Add `UM::Error` class to be used instead of RuntimeError
  - [v] Add optional ring size argument to `UM.new` (for example, a the
        worker thread for the scheduler `blocking_operation_wait` hook does not need
        a lot of depth, so you can basically do `UM.new(4)`)
  - [v] Add debugging code suggested by Samuel
  - [v] Add support for SQPOLL
        https://unixism.net/loti/tutorial/sq_poll.html
  - [v] Add `UM.socketpair`

  - [v] Add more metrics
    - [v] runqueue depth
    - [v] number of pending fibers
    - [v] ops: transient count, free count
    - [v] total fiber switches, total waiting for CQEs

  - [v] Make writev automatically complete partial writes

  - [ ] Add inotify API

    https://www.man7.org/linux/man-pages/man7/inotify.7.html

  - [ ] Better buffer management buffer rings
    - [v] Add `UM#sendv` method (see below)
    - [v] Benchmark `#sendv` vs `#send_bundle` (in concurrent situation)
    - [ ] Benchmark `#read_each` vs `#read` (in concurrent situation)
    - [v] Support for `IO::Buffer`?
    - [ ] Some higher-level abstraction for managing a *pool* of buffer rings
  
  - [ ] Sidecar mode
    - [v] Convert `UM#initialize` to take kwargs
      - [v] `:size` - SQ entries
      - [v] `:sqpoll` - sqpoll mode
      - [v] `:sidecar` - sidecar mode
    - [v] Sidecar implementation
      - [v] sidecar thread
      - [v] futex handling
      - [v] submission logic
    - [ ] 

- [v] UringMachine Fiber::Scheduler implementation
  - [v] Check how scheduler interacts with `fork`.
  - [v] Implement `process_wait` (with `rb_process_status_new`)
  - [v] Implement `fiber_interrupt` hook
  - [v] Add `#address_resolve` hook with same impl as Async:
        https://github.com/socketry/async/blob/ea8b0725042b63667ea781d4d011786ca3658256/lib/async/scheduler.rb#L285-L296
  - [v] Implement other hooks:
    - [v] `#timeout_after`
          https://github.com/socketry/async/blob/ea8b0725042b63667ea781d4d011786ca3658256/lib/async/scheduler.rb#L631-L644
    - [v] `#io_pread`
    - [v] `#io_pwrite`
    - [v] `#io_select`
    - [v] Add timeout handling in different I/O hooks
  - [v] Experiment more with fork:
    - [v] what happens to schedulers on other threads (those that don't make it post-fork)
          - do they get GC'd?
          - do they get closed (`#scheduler_close` called)?
          - are they freed cleanly (at least for UM)?

          ```ruby
          class S
            def respond_to?(sym) = true
          end
          o = S.new
          ObjectSpace.define_finalizer(o, ->(*){ puts 'scheduler finalized' })
          t1 = Thread.new { Fiber.set_scheduler(o); sleep }
          t2 = Thread.new {
            fork { p(t1:, t2:) }
            GC.start
          }

          # output:
          # scheduler finalized
          ```

          So, apparently there's no problem!
  - [v] Implement multi-thread worker pool for `blocking_operation_wait`
        Single thread pool at class level, shared by all schedulers
        With worker count according to CPU count
  - [v] Test working with non-blocking files, it should be fine, and we shouldn't need to reset `O_NONBLOCK`.
  - [v] Implement timeouts (how do timeouts interact with blocking ops?)
  - [v] Implement `#yield` hook (https://github.com/ruby/ruby/pull/14700)
  - [v] Finish documentation for the `FiberScheduler` class
  - [v] Implement `#io_close` hook

  - [v] tests:
    - [v] Wrap the scheduler interface such that we can verify that specific
      hooks were called. Add asserts for called hooks for all tests.
    - [v] Sockets (only io_wait)
    - [v] Files
    - [v] Mutex / Queue
    - [v] Thread.join
    - [v] Process.wait
    - [v] fork
    - [v] system / exec / etc.
    - [v] popen
  - [v] "Integration tests"
    - [v] IO - all methods!
    - [v] queue: multiple concurrent readers / writers
    - [v] net/http test: ad-hoc HTTP/1.1 server + `Net::HTTP` client
    - [v] pipes: multiple pairs of fibers - reader / writer
    - [v] sockets: echo server + many clients

  - [v] Benchmarks
    - [v] UM queue / Ruby queue (threads) / Ruby queue with UM fiber scheduler

          N groups where each group has M producers and O consumers accessing the same queue.

    - [v] UM mutex / Ruby mutex (threads) / Ruby mutex with UM fiber scheduler

      - [v] N groups where each group has M fibers locking the same mutex and
            performing CPU-bound work
      - [v] N groups where each group has M fibers locking the same mutex and
            performing IO-bound work (write to a file)

    - [v] Pipe IO raw UM / Ruby threaded / Ruby with UM fiber scheduler

          N groups where each group has a pair of reader / writer to a pipe

    - [v] Socket IO (with socketpair) raw UM / Ruby threaded / Ruby with UM fiber scheduler

          N groups where each group has a pair of reader / writer to a socketpair

    - [v] Postgres test

- [ ] Ruby Fiber::Scheduler interface
  - [v] Make a PR for resetting the scheduler and resetting the fiber non-blocking flag.
  - [v] hook for close
  - [ ] hooks for send/recv/sendmsg/recvmsg

- [ ] SSL
  - [ ] openssl gem: custom BIO?

    - curl: https://github.com/curl/curl/blob/5f4cd4c689c822ce957bb415076f0c78e5f474b5/lib/vtls/openssl.c#L786-L803

- [ ] UringMachine website
  - [ ] domain: uringmachine.dev
  - [ ] logo: ???
  - [ ] docs (similar to papercraft docs)

- [ ] Uma - web server
  - [ ] child process workers
  - [ ] reforking (following https://github.com/Shopify/pitchfork)
        see also: https://byroot.github.io/ruby/performance/2025/03/04/the-pitchfork-story.html
        - Monitor worker memory usage - how much is shared
        - Choose worker with most served request count as "mold" for next generation
        - Perform GC out of band, preferably when there are no active requests
          https://railsatscale.com/2024-10-23-next-generation-oob-gc/
        - When a worker is promoted to "mold", it:
          - Stops `accept`ing requests
          - When finally idle, calls `Process.warmup`
          - Starts replacing sibling workers with forked workers
        see also: https://www.youtube.com/watch?v=kAW5O2dkSU8
  - [ ] Each worker is single-threaded (except for worker threads)
  - [ ] Rack 3.0-compatible
        see: https://github.com/socketry/protocol-rack
  - [ ] Rails integration (Railtie)
        see: https://github.com/socketry/falcon
  - [ ] Benchmarks
  - [ ] Add to the TechEmpower bencchmarks
