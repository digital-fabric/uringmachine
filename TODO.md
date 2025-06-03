# ops

- [ ] multishot timeout
  - [v] machine.periodically(interval) { ... }
  - [ ] machine.prep_timeout_multishot(interval)

- splice / - tee
- sendto
- recvfrom
- poll_add / poll_remove / poll_multishot / poll_update
- fsync
- mkdir / mkdirat
- link / linkat / unlink / unlinkat / symlink
- rename / renameat
- fadvise
- madvise
- getxattr / setxattr
- send_bundle / recv_bundle (kernel >= 6.10)

# actors

When doing a `call`, we need to provide a mailbox for the response. can this be
automatic?

# streams

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
