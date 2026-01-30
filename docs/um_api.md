# UringMachine API Reeference

## UringMachine Class Methods

- `debug(msg)` - prints a string message to STDERR.
- `kernel_version` - returns the linux kernel version as an integer, e.g. 607 =>
  version 6.7.
- `new(size)` - creates a new UringMachine instance.
- `pidfd_open(pid)` - creates and returns a file descriptor that refers to the
  given process.
- `pidfd_send_signal(pidfd, sig, flags)` - sends a signal to a process
  identified by a pidfd.
- `pipe` - creates a pipe and returns read and write fds.
- `socketpair(domain, type, protocol)` - creates a socket pair and returns the
  two fds.


## UringMachine Instance Methods

- `accept_each(sockfd)  { |fd| ... }` - accepts incoming connections to the
  given server socket in an infinite loop, yielding each one to the given block.
- `accept_into_queue(sockfd, queue)` - accepts incoming connections to the given
  server socket in an infinite loop, pushing each one to the given queue.
- `accept(sockfd)` - accepts an incoming connection, returning its fd.
- `bind(sockfd, host, port)` - binds the given socket to the given address.
- `close_async(fd)` - closes the given fd asynchronously, i.e. <w>ithout waiting
  for the operation to complete.
- `close(fd)` - closes the given fd.
- `connect(sockfd, host, port)` - connects the given socket to the given
  address.
- `getsockopt(sockfd, level, opt)` - returns a socket option value.
- `listen(sockfd)` - starts listening on the given socket.
- `metrics` - returns metrics for the machine.
- `open(pathname, flags)` - opens the given path and returns an fd.
- `pending_fibers` - returns the set of pending fibers, that is, fibers waiting
  for an operation to complete.
- `periodically(interval) { ... }` - runs the given block at regular intervals
  in an infinite loop.
- `poll(fd, mask)` - waits for the given fd to become ready according to the
  given event mask.
- `pop(queue)` - removes and returns a value off the end of the given queue.
- `prep_timeout(interval)` - returns a timeout AsyncOp with the given interval.
- `push(queue, value)` - adds the given value to the end of the given queue.
- `read_each(fd, bgid) { |data| ... }` - reads repeatedly from the given fd
  using the given buffer group id, yielding each chunk of data to the given
  block.
- `read(fd, buffer[, maxlen[, buffer_offset[, file_offset]]])` - reads from the
  given fd int o the given buffer (String or IO::Buffer).
- `recv_each(fd, bgid, flags)` - receives from the given fd using the given
  buffer group id, with the given flags.
- `recv(fd, buffer, maxlen, flags)` - receives from the given fd into the given
  buffer.
- `schedule(fiber, value)` - adds the given fiber to the runqueue with the given
  resume value.
- `select(rfds, wfds, efds)` - selects ready fds from the given readable,
  writable and exeptable fds.
- `send_bundle(fd, bgid, *strings)` - sends a bundle of buffers to the given fd
  using the given buffer group id.
- `send(fd, buffer, len, flags)` - sends to the given fd from the given buffer.
- `sendv(fd, *buffers)` - sends multiple buffers to the given fd.
- `setsockopt(fd, level, opt, value)` - sets a socket option.
- `setup_buffer_ring(size, count)` - sets up a buffer ring and returns the
  buffer group id.
- `shift(queue)` - removes and returns a value from the head of given queue.
- `shutdown_async(fd, how)` - shuts down the given socket fd without blocking.
- `shutdown(fd, how)` - shuts down the given socket fd.
- `size` - returns the number of entries in the submission queue.
- `sleep(duration)` - sleeps for the given duration.
- `snooze` - adds the current fiber to the end of the runqueue and yields
  control to the next fiber in the runqueue.
- `socket(domain, type, protocol, flags)` - creates a socket and returns its fd.
- `statx(dirfd, path, flags, mask)` - returns information for the given path.
- `submit` - submits any unsubmitted operations to the submission queue.
- `switch` - switches to the next fiber in the runqueue.
- `synchronize(mutex)` - synchronizes access to the given mutex.
- `timeout(interval, exception_class) { ... }` - runs the given block, raising
  an exception if the timeout interval has elapsed.
- `unshift(queue, value)` - adds the given value to the beginning of the given
  queue.
- `waitid_status(idtype, id, options)` - waits for the given pid/pidfd and
  returns a `Process::Status`.
- `waitid(idtype, id, options)` - waits for the given pid/pidfd and returns
  `[pid, status]`.
- `wakeup` - wakes up a machine currently waiting for completions.
- `write_async(fd, buffer[, len[, offset]])` - writes to the given fd without
  waiting for completion.
- `write(fd, buffer[, len[, offset]])` - writes to the given fd.
- `writev(fd, *buffers[, file_offset])` - writes multiple buffers to the given
  fd.
- `yield()` - yields control to the next fiber in the runqueue.

