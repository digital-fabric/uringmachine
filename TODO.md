## immediate

## Measuring CPU time for fibers

- use CPU time (CLOCK_THREAD_CPUTIME_ID)
- measure:
  - time each fiber is waiting
  - time each fiber is running
  - time machine is waiting (for CQEs)
  - time machine is running fibers from the runqueue
- can be turned on/off at any time
- no performance impact when off

How can this be implemented:

- `um_get_time_cpu()` function for reading CPU time (CLOCK_THREAD_CPUTIME_ID) as
  double.
- add to `struct um`:

  ```c
  struct um {
    ...
    int profiling_mode;
    double total_time_run;
    double total_time_wait;
    double last_cpu_time;
  }
  ```

- `UM#profile=` to turn it on/off.
- On `machine.profile = true`, reset `total_time_xxx` and `last_cpu_time`

  ```c
  machine->total_time_run = 0;
  machine->total_time_wait = 0;
  machine->last_cpu_time = um_get_time_cpu();
  ```

- when profiling is active:
  - before processing CQEs:

    ```c
    // before
    double cpu_time0;
    VALUE fiber;
    int profiling_mode = machine->profiling_mode;
    if (profiling_mode) {
      fiber = rb_fiber_current();
      cpu_time0 = um_get_time_cpu();
      double elapsed = cpu_time0 - machine->last_cpu_time;
      um_update_fiber_time_run(fiber, cpu_time0, elapsed);
      machine->total_time_run += elapsed;
    }
    process_cqes(...)
    // after
    if (profiling_mode) {
      double cpu_time1 = um_get_time_cpu();
      double elapsed = cpu_time1 - cpu_time0;
      um_update_fiber_last_time(fiber, cpu_time1);
      machine->total_time_wait += elapsed;
      machine->last_cpu_time = cpu_time1;
    }
    ```

  - when doing switching, in `um_process_runqueue_op`:

    ```c
    // before
    double cpu_time;
    VALUE cur_fiber;
    VALUE next_fiber = get_next_fiber(...);
    int profiling_mode = machine->profiling_mode;
    if (profiling_mode) {
      cur_fiber = rb_fiber_current();
      cpu_time = um_get_time_cpu();
      double elapsed = cpu_time - machine->last_cpu_time;
      um_update_fiber_time_run(cur_fiber, cpu_time, elapsed);
      machine->total_time_run += elapsed;
      um_update_fiber_time_wait(next_fiber, cpu_time);
      machine->last_cpu_time = cpu_time;
    }
    do_fiber_transfer(...)
    ```

  - updating fiber time instance vars:

    ```c
    inline void um_update_fiber_time_run(VALUE fiber, double stamp, double elapsed) {
      // VALUE fiber_stamp = rb_ivar_get(fiber, ID_time_last_cpu);
      VALUE fiber_total_run = rb_ivar_get(fiber, ID_time_total_run);
      double total = NIL_P(fiber_total_run) ?
        elapsed : NUM2DBL(fiber_total_run) + elapsed;
      rb_ivar_set(fiber, ID_time_total_run, DBL2NUM(total));
      rb_ivar_set(fiber, ID_time_last_cpu, DBL2NUM(stamp));
    }

    inline void um_update_fiber_time_wait(VALUE fiber, double stamp) {
      VALUE fiber_last_stamp = rb_ivar_get(fiber, ID_time_last_cpu);
      if (likely(!NIL_P(fiber_last_stamp))) {
        double last_stamp = NUM2DBL(fiber_last_stamp);
        double elapsed = stamp - last_stamp;
        VALUE fiber_total_wait = rb_ivar_get(fiber, ID_time_total_wait);
        double total = NIL_P(fiber_total_wait) ?
          elapsed : NUM2DBL(fiber_total_wait) + elapsed;
        rb_ivar_set(fiber, ID_time_total_wait, DBL2NUM(total));
      }
      else
        rb_ivar_set(fiber, ID_time_total_wait, DBL2NUM(0.0));
      rb_ivar_set(fiber, ID_time_last_cpu, DBL2NUM(stamp));
    }
    ```

## Metrics API

- machine metrics: `UM#metrics` - returns a hash containing metrics:

  ```ruby
  {
    size:,          # SQ size (entries)
    total_ops:, # total ops submitted
    total_fiber_switches:, # total fiber switches
    total_cqe_waits:, # total number of CQE waits
    ops_pending:, # number of pending ops
    ops_unsubmitted:, # number of unsubmitted
    ops_runqueue:, # number of ops in runqueue
    ops_free:, # number of ops in freelist
    ops_transient:, # number of ops in transient list
    hwm_pending:, # high water mark - pending ops
    hwm_unsubmitted:, # high water mark - unsubmitted ops
    hwm_runqueue:, # high water mark - runqueue depth
    hwm_free:, # high water mark - ops in free list
    hwm_transient:, # high water mark - ops in transient list
    # when profiling is active
    time_total_run:, # total CPU time running
    time_total_wait:, # total CPU time waiting for CQEs
  }
  ```

- For this we need to add tracking for:
  - runqueue list size
  - transient list size
  - free list size
- Those will be done in um_op.c (in linked list management code)

- All metrics info in kept in

## useful concurrency tools

- debounce

  ```ruby
  debouncer = UM.debounce { }
  ```

## ops

- [ ] multishot timeout
  - [v] machine.periodically(interval) { ... }
  - [ ] machine.prep_timeout_multishot(interval)

- writev
- splice / - tee
- sendto
- recvfrom
- poll_multishot
- fsync
- mkdir / mkdirat
- link / linkat / unlink / unlinkat / symlink
- rename / renameat
- fadvise
- madvise
- getxattr / setxattr
- send_bundle / recv_bundle (kernel >= 6.10)

## actors

When doing a `call`, we need to provide a mailbox for the response. can this be
automatic?

## streams

We're still missing:

- limit on line length in `get_line`
- ability to supply buffer to `get_line` and `get_string`
- allow read to eof, maybe with `read_to_eof`

For the sake of performance, simplicity and explicitness, we change the API as follows:

```ruby
stream.get_line(buf, limit)
# the defaults:
stream.get_line(nil, -1)

stream.get_string(len, buf)
# defaults:
stream.get_string(len, nil)

# and
stream.read_to_eof(buf)
# defaults:
stream.read_to_eof(nil)
```
