## immediate

- Fix all futex value (Queue, Mutex) to be aligned

## Buffer rings - automatic management

- Transition all ops to be heap-allocated (i.e. taken from the freelist)
  - Rework op flags:

    - `OP_F_COMPLETED` - a CQE has been received for the operation
    - `OP_F_TRANSIENT` - not clear, used for OP_SCHEDULE and for async timeout
    - `OP_F_ASYNC` - used for AsyncOp (async timeout), prevents scheduling of fiber
    - `OP_F_CANCELED` - the operation has been cancelled, do not process result or scheduling of fiber
    - `OP_F_IGNORE_CANCELED` - ignore CQE when result = -ECANCELED (otherwise the result is still processed)
    - `OP_F_MULTISHOT` - the op is multishot
    - `OP_F_FREE_ON_COMPLETE` - free the op when processing the CQE
    - `OP_F_RUNQUEUE_SKIP` - skip runqueue entry, used for multishot ops where the op is already put on the runqueue
    - `OP_F_SELECT_POLL*` - used for `#select` to mark which op is used for which array of fds

    Observations:

    - There's some duplication of functionality between `ASYNC`, `TRANSIENT`,
      `FREE_ON_COMPLETE`.
    - The current way multishot results are treated is incorrect and can
      potentially cause a segfault, the behaviour is incorrect because when
      multiple CQEs are received for the same op, it is in fact going to be
      scheduled multiple times, which would break the linked-list nature of the
      runqueue.
    - The flags don't clearly reflect the state of the operation on the
      application side, or the kernel side. Moving to flags that mirror the
      state on the two sides would lead to better code and easier extensibility.
    - We're missing a flag to signify when a CQE is definitely done on the
      kernel side (at least for a multishot op).

    So:

    - No more stack-allocated ops, all ops are heap-allocated.
    - `OP_F_MULTISHOT` - multishot mode
    - `OP_F_CQE_SEEN` - CQE processed
    - `OP_F_CQE_DONE` - CQE processed, for multishot this means lack of CQE_F_MORE
    - `OP_F_SCHEDULED` - op is on runqueue (instead of `RUNQUEUE_SKIP`)
    - `OP_F_CANCELED` - op is cancelled (CQE should not be processed, should not be put on the runqueue.

    - The app/kernel double lifecycle and retaining should be management instead
      by using a ref_count.
      - Retained by the app (until after fiber resume) - increment by 1
      - Retained by the kernel (SQE submitted) - increment by 1
      - So, upon creating the SQE, set the ref_count to 2
      - When the op is put on the runqueue, increment the ref_count
      - When the op is pulled from the runqueue, decrement the ref_count
      - When the app is done with the op (whether cancelled or completed),
        decrement the ref_count
      - When the `OP_F_CQE_DONE` flag is set, decrement the ref_count
      - After decrementing, if the ref_count is zero, the op is freed and put
        back on the freelist.

    For a `OP_SCHEDULE` op (on calling `#schedule`):

    - app does not need to retain, kernel does not need to retain, just put the
      op on the runqueue, which means ref_count = 1
    - when the op is pulled from the runqueue, decrement the ref_count, so
      ref_count = 0, which means free the op.

    For a normal read operation:

    - create SQE
      - flags = 0
      - ref_count = 2
    - CQE is processed
      - flags |= OP_F_CQE_SEEN | OP_F_CQE_DONE
      - ref_count-- (ref_count = 1)
      - flags |= OP_F_SCHEDULED
      - ref_count++ (ref_count = 2)
    - op is pulled from runqueue
      - flag &= ~OP_F_SCHEDULED
      - ref_count-- (ref_count = 1)
    - fiber is resumed
      - op result is processed
      - ref_count-- (ref_count = 0, free!)

    For a cancelled read operation:

    - create SQE
      - flags = 0
      - ref_count = 2
    - fiber is resumed with exception
      - (flags & OP_F_CQE_DONE) == 0
      - cancel the op
      - flags |= OP_F_CANCELED
      - ref_count-- (ref_count = 1)
    - CQE is processed
      - ref_count-- (ref_count = 0, free!)
    
    Multishot accept operation:

    - create SQE
      - flags = OP_F_MULTISHOT
      - ref_count = 2
    - CQE is processed
      - check CQE_F_MORE (true)
      - flags |= OP_F_CQE_SEEN
      - CQE_F_MORE is set, so don't decrement ref_count
      - flags |= OP_F_SCHEDULED
      - ref_count++ (ref_count = 3)
    - CQE is processed
      - check CQE_F_MORE (true)
      - flags |= OP_F_CQE_SEEN
      - check OP_F_SCHEDULED (already set)
        - skip scheduling (ref_count = 3)
    - op is pulled from runqueue
      - flag &= ~OP_F_SCHEDULED
      - ref_count-- (ref_count = 2)
    - fiber is resumed
      - op results are processed
      - yield control
    - fiber is resumed with exception
      - cancel the op
      - flags |= OP_F_CANCELED
      - ref_count-- (ref_count = 1)
    - CQE is processed
      - check CQE_F_MORE (false)
      - flags |= OP_F_CQE_DONE
      - ref_count-- (ref_count = 0, free!)

    AsyncOp:

    - create SQE
      - flags = 0
      - ref_count = 2
      - op->fiber = Qnil
    - CQE processed
      - fiber is nil, so don't schedule
      - flags |= OP_F_CQE_SEEN | OP_F_CQE_DONE
      - ref_count-- (ref_count = 1)
    - `AsyncOp#await` is called
      - if OP_F_CQE_DONE set
        - return
      - if OP_F_CQE_DONE not set
        - set op->fiber
        - wait for CQE
        - if resumed with exception
          - cancel the op
          - flags |= OP_F_CANCELED
          - return
    - `AsyncOp->free`
      - if OP_F_CQE_DONE not set
        - cancel the op
        - flags |= OP_F_CANCELED
        - ref_count-- (ref_count = 1)
      - if OP_F_CQE_DONE set
        - ref_count-- (ref_count = 0, free!)

- Take the buffer_pool branch, rewrite it
- Allow multiple stream modes:
  - :buffer_pool - uses buffer rings
  - :ssl - read from an SSL connection (`SSLSocket`)
  - :io - read from an `IO`

The API will look something like:

```ruby
# The mode is selected automatically according to the given target

stream = UM::Stream.new(fd) # buffer_pool mode

stream = UM::Stream.new(ssl_sock) # ssl mode

stream = UM::Stream.new(conn) # io mode
```

## Balancing I/O with the runqueue

- in some cases where there are many entries in the runqueue, this can
  negatively affect latency. In some cases, this can also lead to I/O
  starvation. If the runqueue is never empty, then SQEs are not submitted and
  CQEs are not processed.
- So we want to limit the number of consecutive fiber switches before processing
  I/O.
- Some possible approaches:

  1. limit consecutive switches with a parameter
  2. limit consecutive switches relative to the runqueue size and/or the amount
     of pending SQEs
  3. an adaptive algorithm that occasionally measures the time between I/O
     processing iterations, and adjusts the consecutive switches limit?

- We also want to devise some benchmark that measures throughput / latency with
  different settings, in a situation with very high concurrency.

## useful concurrency tools

- debounce

  ```ruby
  debouncer = machine.debounce { }
  ```

- read multiple files

  ```ruby
  # with a block
  machine.read_files(*fns) { |fn, data| ... }

  # without a block
  machine.read_files(*fns) #=> { fn1:, fn2:, fn3:, ...}
  ```

## polyvalent select

- select on multiple queues (ala Go)
- select on mixture of queues and fds

(see also simplified op management below)

## simplified op management

Op lifecycle management can be much much simpler

- make all ops heap-allocated
- clear up state transitions:

  - kernel-side state: unsubmitted, submitted, completed, done (for multishot ops)
  - app-side state: unsubmitted, submitted, ...


## ops

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
