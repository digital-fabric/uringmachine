## immediate

- Add `IO#http_xxx` methods
  - `#http_read_request_headers()`
  - `#http_read_body(content_length)` (-1 means chunked TE)

- Add tests for support for Set in `machine#await`
- Add tests for support for Set, Array in `machine#join`
- Add `UM#read_file` for reading entire file
- Add `UM#write_file` for writing entire file

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
  debouncer = machine.debounce { ... }
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

- sendto
- recvfrom
- poll_multishot
- mkdir / mkdirat
- link / linkat / unlink / unlinkat / symlink
- rename / renameat
- fadvise
- madvise
- getxattr / setxattr

## actors

When doing a `call`, we need to provide a mailbox for the response. can this be
automatic?

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

## Character scanning in UM::IO

```c
// bitmaps for character types can be generated with a bit of Ruby:
//
// def t(r); (0..255).map { [it].pack('c') =~ r ? 1 : 0 }; end
// def tn(r); (0..255).map { [it].pack('c') =~ r ? 0 : 1 }; end
// def u64(bits); bits.reverse.join.to_i(2); end
// def p(a); a.each_slice(64).map { u64(it) }; end

// usage:
//
//   p(t(/[a-zA-Z0-9]/)).map { format('%016X', it) }


// /[a-zA-Z0-9]/
uint64_t alpha_numeric[] = [
  0x000000000000FFC0,
  0x7FFFFFE07FFFFFE0,
  0x0000000000000000,
  0x0000000000000000
];

// HTTP method: /[a-zA-Z]/    (3-12 characters)
// header-key:  /[a-zA-Z\-]/  ()
// path:        /^($/

// check if character is in bitmap
inline int test_char(char c, uint64 *bitmap) {
  return bitmap[c / 64] & (1UL << (c % 64));
}


```
