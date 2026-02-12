# UringMachine - High Performance Concurrency for Ruby Using io_uring

https://www.papercall.io/talks/413880/children/413881

## Structure

- Introduction: the name (1)
- What is io_uring (2)
- How UringMachine works (4)
  - The liburing API
  - Submitting an operation
  - Fiber switching and the runqueue
  - Waiting for and processing CQEs
  - The UringMachine model:
    - exhaust all CPU-bound work, submit I/O work to kernel
    - Wait for and process completions
    - repeat
- The UringMachine API (7)
  - More or less equivalent to the Unix I/O API
  - Synchronization: `UM::Mutex`, `UM::Queue`
  - Useful abstractions for multishot operations
  - OpenSSL support
    - Custom BIO
  - Streams / Automatic buffer management
  - Utilities and advanced usage: `.inotify`, `.pipe`, `#splice`
- Timeouts and cancellations (4)
  - Timeout - the basic design
  - The challenges of cancellation
    - double life cycle
    - holding on to buffers
    - ensuring the lifetime of relevant objects when doing an async operation.
      - the TRANSIENT list of ops, for async operations (e.g. `#write_async` -
        how to do we hold on to the write buffer.)
      - how in general we mark the objects involved in async 
- Integration with the Ruby ecosystem (6)
  - The Fiber::Scheduler interface: the good, the bad and the ugly
  - Dealing with CPU-bound workloads
    - SQLite and Extralite in particular (`on_progress` handler)
- Performance (3)
  - Benchmarks
  - CPU-bound / IO-bound: how UringMachine deals with mixed workloads
- Applications (3)
  - TP2 / Syntropy
  - Uma - Rack-compatible app server
    - Compare performance of a Rails app on Uma / Falcon / Puma
- Future directions (3)
  - Further contributions to Ruby:
    - Support for Socket I/O in Fiber::Scheduler
  - More stress testing, prove stability 
  - More performance research
  - Make Uma into a first-class app server for Ruby
  - Introduce higher-level 
