## Immediate

- `UM::IO`: Implement automatic failover to `IO_MODE` mode when doing a multishot read (in
  `IO_FD`) and getting a `-EBADFD` error.

## CSP

How amenable is UringMachine to CSP-style programming?

https://go.dev/talks/2012/concurrency.slide#50

also:

https://www.youtube.com/watch?v=BcHI3U0FuoY

```ruby
def do_search
  q = UM::Queue.new
  @machine.spin { @machine.push(q, query(web1, web2)) }
  @machine.spin { @machine.push(q, query(img1, img2)) }
  @machine.spin { @machine.push(q, query(vid1, vid2)) }
  results = []
  @machine.timeout(0.08, Timeout::Error) {
    3.times { results << @machine.shift(q) }
  }
  results
end
```

But maybe we can build an abstraction on top of it:

```ruby
def do_search
  c = UM::Channel.new(@machine)
  @machine.spin { c << query(web1, web2) }
  @machine.spin { c << query(img1, img2) }
  @machine.spin { c << query(vid1, vid2) }
  results = []
  @machine.timeout(0.08, Timeout::Error) {
    3.times { results << c.next }
  }
  results
end
```

## Enahance working with queues

- Add `#shift_each`

  ```ruby
  machine.shift_each(queue) { |v| ... }
  ```

```ruby
q1 = UM::Queue.new
q2 = UM::Queue.new

# return value is array containing queue and shifted value
machine.multi_shift(q1, q2) #=> [q, v]
```

- Add support for shifting from a UM::IO

This lets us treat an IO as a channel/queue:

```ruby
io = machine.io(fd)
queue = UM::Queue.new

q, v = machine.multi_shift(io, queue)
```

## File copying with IO

```ruby
# instead of:
io_foo.read_each { machine.write(some_other_fd, it) }

# or:
io_foo.read_each { io_bar.write(it) }

# add:
io_foo.copy_each(some_other_fd || io)
```

Also, we can add a specialized case: sending chunks from a given IO:

```ruby
conn.http1_write_chunks_from_io(file_io)
```

## Character scanning in UM::IO

- Refactor character scanning
  - functions/macros for iterating over segments

  - SIMD?
    - prep work for SIMD character scanning
    - we'll probably need a bit of helper code for dealing with SIMD
    - a good overview here: https://mitchellh.com/writing/everyone-should-know-simd
    - SIMD prep work should be done once before iterating over segments

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

## IO methods

- IO methods:
  ```ruby
  io.read_int_be(length)
  io.read_uint_be(length)
  io.read_int_le(length)
  io.read_uint_le(length)
  ```

- It might be interesting to have a DSL where we can define a frame/header structure:

  ```ruby
  http2_transform = UM::IO.transform do
    length    uint(24)
    type      uint(8)
    flags     uint(8)
    stream_id uint(32)
  end

  frame = io.read(http2_transform)
  raise if frame[:length] > MAX_LENGTH
  raise if stream_id &  0x80000000
  ```

- Add `IO#http1_xxx` methods
  - `#http1_read_request_headers()`
  - `#http1_read_body(content_length)` (-1 means chunked TE)

- Add tests for support for Set in `machine#await`
- Add tests for support for Set, Array in `machine#join`
  - Add `UM#read_file` for reading entire file
- Add `UM#write_file` for writing entire file

## SSL custom BIO:

- release GVL before calling `SSL_read`, `SSL_write`
- reacquire GVL (`rb_thread_call_with_gvl`) in `um_bio_read`, `um_bio_write`

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
- we might want to rename `#select` to `#fdselect`

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

## Other abstractions

- Happy eyeballs connect

  ```ruby
  # addrs: [['1.1.1.1', 80], ['2.2.2.2', 80]]
  #        ['1.1.1.1:80', '2.2.2.2:80']
  tcp_connect_he(*addrs)
  ```

## Data-Oriented Design

see: https://www.youtube.com/watch?v=uI1Yx1CJAB0

The current UM implementation is a "forest of pointers". It is also quite
complex, to a large extent because the design has evolved over a long period of
time. The automatic buffers feature in particular has necessitated many changes
to the original design, and the code shows this.

Would it be possible to move to a data-oriented design where instead of pointers
we use indexes into dynamic arrays of:

- `um_op`
- `um_op_result`
- `um_segment`
- `um_buffer`

In and of itself, switching from pointers to indexes is not that hard. What's
more challenging is the fact that in UM things are happening concurrently and
asynchronously, which means different `um_op`s (for example) have different
lifetimes, so we can't just treat a dynamic array of `um_op` as something that
just grows. A `um_op` in the middle of the array might be freed up, so we need
to track lifetimes and free slots. We also need to have some kind of abstraction
for a linked list that's index-based, and at the same time we need to track free
slots with a free list.

We use the concept of a store. The store itself doesn't deal with tracking the
list head/tail. This is done at a level above.

```c
typedef uint32_t um_op_idx;
#define UM_OP_NIL = (uint32_t)-1;

struct um_op_store {
  struct um_op *store;
  size_t size;
  size_t used_count;
  um_op_idx free_head;
};

void um_op_store_init(struct um_op_store *list, size_t initial_size) {
  list->size = initial_size;
  list->store = malloc(sizeof(um_op) * initial_size);
  memset(list->store, 0, sizeof(um_op) * initial_size);
  if (!(list->store)) rb_raise("Failed to allocate");

  list->used_count = 0;
  list->free_head = 0;

  for (um_op_idx i = 0; i < (initial_size - 1); i++)
    list->store[i].next = i + 1;
}

void um_op_store_deinit(struct um_op_store *list) {
  free(list->store);
}

struct um_op *um_op_store_get(struct um_op_store *list, um_op_idx idx) {
  // TODO: add checkss
  return list->store + idx;
}

um_op_idx um_op_store_alloc(struct um_op_store *list) {
  if (list->free_head == UM_OP_NIL) {
    // reallocate list with size * 2
    // set free_head
  }

  um_op_idx idx = list->free_head;
  list->free_head = list->store[idx].next;
  list->store[idx].next = UM_OP_NIL;
  list->used_count++;
  return idx;
}

void um_op_store_free(struct um_op_store *list, um_op_idx idx) {
  list->store[idx].next = list->free_head;
  list->free_head = idx;
  list->used_count--;
}
```
