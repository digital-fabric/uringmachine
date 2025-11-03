## immediate

- make a reproducer for segfault on timeout, spin lots of fibers where a timeout
  wraps a #shift call (from an empty queue).
- see also: https://mensfeld.pl/2025/11/ruby-ffi-gc-bug-hash-becomes-string/

Analysis:

- The segfault is related to timeouts
- Looking at process_runqueue_op (um.c):

  ```c
  inline VALUE process_runqueue_op(struct um *machine, struct um_op *op) {
    VALUE fiber = op->fiber;
    VALUE value = op->value;

    // on timeout, the op flags are changed to turn on OP_F_TRANSIENT
    if (unlikely(op->flags & OP_F_TRANSIENT))
      // here the op is freed, so the value is not visible to the GC anymoore
      um_op_free(machine, op);

    // if a GC occurs here, we risk a segfault

    // value is used
    return rb_fiber_transfer(fiber, 1, &value);
  }
  ```

- So, a possible solution is to put a `RB_GC_GUARD` after the `return`.
- But first, I want to be able to reproduce it. We can start by setting
  `GC.stress = true` on tests and see if we segfault.

## FiberScheduler implementation

Some resources:

- https://github.com/socketry/async/blob/main/context/getting-started.md
- https://github.com/socketry/async/blob/main/context/scheduler.md
- https://github.com/socketry/async/blob/main/lib/async/scheduler.rb#L28
- 

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
