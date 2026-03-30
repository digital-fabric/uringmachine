## immediate

- Add tests for support for Set in `machine#await`
- Add tests for support for Set, Array in `machine#join`
- Add `UM#read_file` for reading entire file
- Add `UM#write_file` for writing entire file
- Rename stream methods: `:fd`, `:socket`, `:ssl`

## Improving streams

One wart of the stream API is that it's only for reading, so if we want to
implement a protocol where we read and write to a target fd, we also need to
keep the fd around or call `stream.target` every time we want to write to it,
*and* we don't have a transport-agnostic write op.

What if instead of `Stream` we had something called `Link`, which serves for
both reading and writing:

```ruby
conn = machine.connection(fd)
while l = conn.read_line
  conn.write(l, '\n')
end
# or:
buf = conn.read(42)
```

RESP:

```ruby
conn.resp_write(['foo', 'bar'])
reply = conn.resp_read
```

HTTP:

```ruby
r = conn.http_read_request
conn.http_write_response({ ':status' => 200 }, 'foo')

# or:
conn.http_write_request({ ':method' => 'GET', ':path' => '/foo' }, nil)
```

Plan of action:

- Rename methods:
  - [v] rename `#read_line` to `#read_line`
  - [v] rename `#read` to `#read`
  - [v] rename `#read_to_delim` to `#read_to_delim`
  - [v] rename `#each` to `#read_each`
  - [v] rename `#resp_decode` to `#resp_read`
- Rename modes:
  - [v] :fd to :fd
  - [v] :socket to :socket
  - [v] auto detect SSL
- Rename `Stream` to `Connection`
- Add methods:
  - `#write(*bufs)`
  - `#resp_write(obj)`

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

- happy eyeballs algo for TCP connect

- read multiple files

  ```ruby
  # with a block
  machine.read_files(*fns) { |fn, data| ... }

  # without a block
  machine.read_files(*fns) #=> { fn1:, fn2:, fn3:, ...}
  ```

- more generally, a DSL for expressing batch operations:

  ```ruby
  result = machine.batch do |b|
    fns.each { b[it] = read_file(b, it) }
  end
  #=> { fn1 => data1, fn2 => data2, ... }

  # we can also imagine performing operations in sequence using linking:
  result = machine.batch {
    m.
  }

  end
  ```

## polyvalent select

- select on multiple queues (ala Go)
- select on mixture of queues and fds
- select on fibers:
  - select fibers that are done
  - select first done fiber

## ops still not implemented

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

##

## Syntax / pattern for launching/supervising multiple operations

Select (see above):

```ruby
# select
machine.join_select(*fibers) #=> [result, fiber]
machine.shift_select(*queues) #=> [result, queue]
```

## Other abstractions

- Happy eyeballs connect

  ```ruby
  # addrs: [['1.1.1.1', 80], ['2.2.2.2', 80]]
  #        ['1.1.1.1:80', '2.2.2.2:80']
  tcp_connect_he(*addrs)
  ```
