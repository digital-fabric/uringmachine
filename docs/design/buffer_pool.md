# UringMachine Buffer Pool

One of the interesting recent features in io_uring is support for buffer rings.
A buffer ring is a structure that is shared between the application and the
kernel. The application can add buffers to the buffer ring to be used by the
kernel to perform multishot read/recv.

https://www.man7.org/linux/man-pages/man3/io_uring_setup_buf_ring.3.html
https://www.man7.org/linux/man-pages/man3/io_uring_prep_recv.3.html

On recent kernels (>=6.12), io_uring also supports incremental buffer usage,
which means it can partially consume buffers, such that buffer space will not be
wasted in case of a short read/recv.

This documents the API and implementation details of a buffer pool that
automatically manages multiple buffer rings, and allows partial buffer
consumption and reuse. We also provide a way to integrate this feature with
UringMachine *streams*, in effect switching streams from relying on their own
buffers, to using managed buffers from the buffer pool.

## Design

### The API

- The buffer pool is created and managed automatically. No API is involved.

- To use the buffer pool, two dedicated APIs are added:

  - `UM#stream_read(fd) { |stream| ... }`
  - `UM#stream_recv(fd) { |stream| ... }`

- The two APIs work equivalently, where they start a multishot read/recv
  operation, repeating it if necessary, until the given block returns, the fd is
  closed, or an exception is encountered.

- The stream instance provided to the given block is created automatically, and
  is used to interact with the data read/received as it becomes available. The
  stream instance methods may block until enough data is available in the
  relevant buffers.
  
   Example:

  ```ruby
  machine.stream_recv(fd) do |stream|
    loop do
      line = stream.get_line(max: 60)
      if (size = parse_size(line))
        data = stream.read(size)
        process_data(data)
      else
        raise "Protocol error!"
      end
    end
  end
  ```

- Since there is some overhead for setting up streams and multishot operations,
  this API is intended for use in long-running connections, e.g. HTTP, Redis,
  PostgreSQL, etc.

- Right now, streams provide support for reading lines (for line-oriented
  protocols), reading data of arbitrary size, and decoding RESP (Redis protocol)
  messages. In the future, we may add more built-in support for decoding other
  protocols, such as HTTP/1.1, HTTP/2, Websocket etc.

### The buffer pool

- Each UringMachine instance has 1 associated buffer pool.

- A buffer pool manages up to 64 buffer groups.

- Each buffer group has its own associated buffer ring, and 64 buffers. The
  buffer size is fixed at 64KB, for a total size of 64x64KB = 4MB.

- The maximum amount of buffers in a buffer pool is 64X64 = 4096, for a total
  size of 64X4MB = 256MB.

- The buffer pool is responsible for selecting a buffer group for each multishot
  read/recv operation, according to the number of buffers available to the
  kernel.

- The buffer pool is responsible for setting up additional buffer groups (up to
  64) as needed.

- The buffer pool is responsible for maintaining the state of each buffer, and
  whether it is currently committed to the kernel, or available to the
  application.

### Streams

- A stream is automatically created upon a call to `#stream_read` or
  `#stream_recv`.

- The stream provides methods for the application to consume incoming data.

- The stream holds zero or more *segments* of data that are added to the stream
  as more data becomes available through CQEs.

- The different methods scan through the different segments, potentially
  blocking until more segments arrive, and copies data into strings or other
  data structures according to the APIs used.

- Each stream holds a cursor that tells it from which offset and in which
  segment data is currently to be consumed.

- As segments are consumed by the application, the underlying buffers are
  committed back to the kernel to be reused in subsequent CQEs, providing no
  segment of which is currently used by a stream.

### Principle of Operation

- When a multishot operation is started, a buffer group is selected, and its id
  is provided in the corresponding SQE. In case when the buffer group is
  exhausted (no more buffers are available to the kernel), the multishot
  operation is automatically restarted with a newly selected buffer group.
  Buffer groups are created automatically as needed if the currently existing
  buffer groups are exhausted.

- When one or more CQEs are encountered for the multishot operation, the
  corresponding fiber is resumed, and the multishot results are processed into
  segments that are added to the stream, to be eventually processed according to
  the APIs used. Importantly, it is the calls to stream methods that drive the
  eventual consumption of buffer segments. That is, many pieces of data may be
  pending actual processing.

- The actual submission of multishot operations is driven by usage of the
  different stream APIs and the need for more data.

### Data structures

```c
#define UM_BUFFER_POOL_MAX_GROUPS 64
#define UM_BUFFER_GROUP_SIZE 64
#define UM_BUFFER_SIZE (1 << 16)

/*
  A buffer segment represents a contiguous sequence of bytes coming from a
  managed buffer. buffer segments are arranged in a linked list, each one
  pointing to the next. In order to minimize allocations, those structs are
  reused, and when no longer needed they are added to a freelist on the buffer
  pool.
*/
struct um_buffer_segment {
  struct um_buffer_segment *next;
  uint8_t bgid; // buffer group id
  uint8_t bid; // buffer id
  void *ptr;
  size_t len;
}

/*
  A stream is made of zero or more buffer segments.
*/
struct um_stream {
  struct um *machine;
  struct um_buffer_segment *head;
  struct um_buffer_segment *tail;
}

/*
  A managed buffer. It has a fixed size of 64KB
*/
struct um_buffer {
  size_t ofs; // current offset for partial consumption (incremented by CQE result)
  uint16_t commited; // is buffer available to the kernel
  uint16_t ref_count; // how many segments currently use the buffer
  uint8_t data[UM_BUFFER_SIZE]; // buffer space
}

struct um_buffer_group {
  struct io_uring_buf_ring *ring;
  uint commited_count;
}

/*
  A buffer pool used for managing buffers.
*/
struct um_buffer_pool {
  struct um_buffer_group[UM_BUFFER_POOL_MAX_GROUPS];
  uint16_t buffer_group_count;
  struct um_buffer_segment *free_list;
}
```
