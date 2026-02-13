## immediate

- Fix all futex value (Queue, Mutex) to be properly aligned

## Buffer rings - automatic management

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
