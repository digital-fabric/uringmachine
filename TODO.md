## immediate

## buffer rings - automatic management

```ruby
# completely hands off
machine.read_each(fd) { |str| ... }

# what if we want to get IO::Buffer?
machine.read_each(fd, io_buffer: true) { |iobuff, len| ... }
```

## write/send multiple buffers at once

This is done as vectored IO:

```ruby
machine.writev(fd, buf1, buf2, buf3)

# with optional file offset:
machine.writev(fd, buf1, buf2, buf3, 0)

# for the moment it won't take flags
machine.sendv(fd, buf1, buf2, buf3)
```

## useful concurrency tools

- debounce

  ```ruby
  debouncer = UM.debounce { }
  ```



## polyvalent select

- select on multiple queues (ala Go)
- select on mixture of queues and fds

## ops

- [ ] multishot timeout
  - [v] machine.periodically(interval) { ... }
  - [ ] machine.prep_timeout_multishot(interval)

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
