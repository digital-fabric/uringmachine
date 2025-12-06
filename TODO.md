## immediate

- Add debugging code suggested by Samuel:

  https://github.com/search?q=repo%3Asocketry%2Fio-event+%22if+%28DEBUG%22&type=code

```ruby
enum {
  DEBUG = 1,
};

const char * op_kind_name(enum op_kind kind) {
  switch (kind) {
    case OP_TIMEOUT: return "OP_TIMEOUT";
    case OP_SCHEDULE: return "OP_SCHEDULE";
    case OP_SLEEP: return "OP_SLEEP";
    case OP_OPEN: return "OP_OPEN";
    case OP_READ: return "OP_READ";
    case OP_WRITE: return "OP_WRITE";
    case OP_WRITE_ASYNC: return "OP_WRITE_ASYNC";
    case OP_CLOSE: return "OP_CLOSE";
    case OP_CLOSE_ASYNC: return "OP_CLOSE_ASYNC";
    case OP_STATX: return "OP_STATX";
    case OP_ACCEPT: return "OP_ACCEPT";
    case OP_RECV: return "OP_RECV";
    case OP_SEND: return "OP_SEND";
    case OP_SEND_BUNDLE: return "OP_SEND_BUNDLE";
    case OP_SOCKET: return "OP_SOCKET";
    case OP_CONNECT: return "OP_CONNECT";
    case OP_BIND: return "OP_BIND";
    case OP_LISTEN: return "OP_LISTEN";
    case OP_GETSOCKOPT: return "OP_GETSOCKOPT";
    case OP_SETSOCKOPT: return "OP_SETSOCKOPT";
    case OP_SHUTDOWN: return "OP_SHUTDOWN";
    case OP_SHUTDOWN_ASYNC: return "OP_SHUTDOWN_ASYNC";
    case OP_POLL: return "OP_POLL";
    case OP_WAITID: return "OP_WAITID";
    case OP_FUTEX_WAIT: return "OP_FUTEX_WAIT";
    case OP_FUTEX_WAKE: return "OP_FUTEX_WAKE";
    case OP_ACCEPT_MULTISHOT: return "OP_ACCEPT_MULTISHOT";
    case OP_READ_MULTISHOT: return "OP_READ_MULTISHOT";
    case OP_RECV_MULTISHOT: return "OP_RECV_MULTISHOT";
    case OP_TIMEOUT_MULTISHOT: return "OP_TIMEOUT_MULTISHOT";
    case OP_SLEEP_MULTISHOT: return "OP_SLEEP_MULTISHOT";
    default: return "UNKNOWN_OP_KIND";
  }
}

inline struct io_uring_sqe *um_get_sqe(struct um *machine, struct um_op *op) {
  if (DEBUG) fprintf(stderr, "-> um_get_sqe: op->kind=%s unsubmitted=%d pending=%d total=%lu\n",
    op ? op_kind_name(op->kind) : "NULL",
    machine->unsubmitted_count,
    machine->pending_count,
    machine->total_op_count
  );
  ...
}

static inline void um_process_cqe(struct um *machine, struct io_uring_cqe *cqe) {
  struct um_op *op = (struct um_op *)cqe->user_data;

  if (DEBUG) {
    if (op) fprintf(stderr, "<- um_process_cqe: op %p kind %s flags %d cqe_res %d cqe_flags %d pending %d\n",
      op, op_kind_name(op->kind), op->flags, cqe->res, cqe->flags, machine->pending_count
    );
    else fprintf(stderr, "<- um_process_cqe: op NULL cqe_res %d cqe_flags %d pending %d\n",
      cqe->res, cqe->flags, machine->pending_count
    );
  }
  ...
}

```



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
