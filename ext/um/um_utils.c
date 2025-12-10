#include "um.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <ruby/io/buffer.h>
#include <time.h>

inline struct __kernel_timespec um_double_to_timespec(double value) {
  double integral;
  double fraction = modf(value, &integral);
  struct __kernel_timespec ts;
  ts.tv_sec = integral;
  ts.tv_nsec = floor(fraction * 1000000000);
  return ts;
}

inline double um_timestamp_to_double(__s64 tv_sec, __u32 tv_nsec) {
  return (double)tv_sec + ((double)tv_nsec) / 1000000000;
}

inline double um_get_time_cpu() {
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return -1.0;

  return um_timestamp_to_double(ts.tv_sec, ts.tv_nsec);
}

inline double um_get_time_monotonic() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1.0;

  return um_timestamp_to_double(ts.tv_sec, ts.tv_nsec);
}


#define RAISE_EXCEPTION(e) rb_funcall(e, ID_invoke, 0);

inline int um_value_is_exception_p(VALUE v) {
  return rb_obj_is_kind_of(v, rb_eException) == Qtrue;
}

inline VALUE um_raise_exception(VALUE e) {
  static ID ID_raise = 0;
  if (!ID_raise) ID_raise = rb_intern("raise");

  return rb_funcall(rb_mKernel, ID_raise, 1, e);
}

inline void um_raise_on_error_result(int result) {
  if (unlikely(result < 0)) rb_syserr_fail(-result, strerror(-result));
}

inline void * um_prepare_read_buffer(VALUE buffer, ssize_t len, ssize_t ofs) {
  if (TYPE(buffer) == T_STRING) {
    size_t current_len = RSTRING_LEN(buffer);
    if (len == -1) len = current_len;
    if (ofs < 0) ofs = current_len + ofs + 1;
    size_t new_len = len + (size_t)ofs;

    if (current_len < new_len)
      rb_str_modify_expand(buffer, new_len);
    else
      rb_str_modify(buffer);
    return RSTRING_PTR(buffer) + ofs;
  }
  else if (IO_BUFFER_P(buffer)) {
    void *base;
    size_t size;
    rb_io_buffer_get_bytes_for_writing(buffer, &base, &size); // writing *to* buffer
    if (len == -1) len = size;
    if (ofs < 0) ofs = size + ofs + 1;
    size_t new_size = len + (size_t)ofs;

    if (size < new_size) {
      rb_io_buffer_resize(buffer, new_size);
      rb_io_buffer_get_bytes_for_writing(buffer, &base, &size);
    }
    return base + ofs;
  }
  else
    um_raise_internal_error("Invalid buffer provided");
}

static inline void adjust_read_buffer_len(VALUE buffer, int result, ssize_t ofs) {
  if (TYPE(buffer) == T_STRING) {
    rb_str_modify(buffer);
    unsigned len = result > 0 ? (unsigned)result : 0;
    unsigned current_len = RSTRING_LEN(buffer);
    if (ofs < 0) ofs = current_len + ofs + 1;
    rb_str_set_len(buffer, len + (unsigned)ofs);
  }
  else if (IO_BUFFER_P(buffer)) {
    // do nothing?
  }
}

inline void um_update_read_buffer(struct um *machine, VALUE buffer, ssize_t buffer_offset, __s32 result, __u32 flags) {
  if (!result) return;

  adjust_read_buffer_len(buffer, result, buffer_offset);
}

inline void um_get_buffer_bytes_for_writing(VALUE buffer, const void **base, size_t *size) {
  if (TYPE(buffer) == T_STRING) {
    *base = RSTRING_PTR(buffer);
    *size = RSTRING_LEN(buffer);
  }
  else if (IO_BUFFER_P(buffer))
    rb_io_buffer_get_bytes_for_reading(buffer, base, size); // reading *from* buffer
  else
    um_raise_internal_error("Invalid buffer provided");
}

int um_setup_buffer_ring(struct um *machine, unsigned size, unsigned count) {
  if (machine->buffer_ring_count == BUFFER_RING_MAX_COUNT)
    um_raise_internal_error("Cannot setup more than BUFFER_RING_MAX_COUNT buffer rings");

  struct buf_ring_descriptor *desc = machine->buffer_rings + machine->buffer_ring_count;
  desc->buf_count = count;
  desc->buf_size = size;
  desc->br_size = sizeof(struct io_uring_buf) * desc->buf_count;
  desc->buf_mask = io_uring_buf_ring_mask(desc->buf_count);

	void *mapped = mmap(
    NULL, desc->br_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0
  );
  if (mapped == MAP_FAILED)
    um_raise_internal_error("Failed to allocate buffer ring");

  desc->br = (struct io_uring_buf_ring *)mapped;
  io_uring_buf_ring_init(desc->br);

  unsigned bg_id = machine->buffer_ring_count;
  int ret;
  desc->br = io_uring_setup_buf_ring(&machine->ring, count, bg_id, 0, &ret);
  if (!desc->br) {
    munmap(desc->br, desc->br_size);
    rb_syserr_fail(-ret, strerror(-ret));
  }

  if (size > 0) {
    if (posix_memalign(&desc->buf_base, 4096, desc->buf_count * desc->buf_size)) {
      io_uring_free_buf_ring(&machine->ring, desc->br, desc->buf_count, bg_id);
      um_raise_internal_error("Failed to allocate buffers");
    }

    void *ptr = desc->buf_base;
    for (unsigned i = 0; i < desc->buf_count; i++) {
	  	io_uring_buf_ring_add(desc->br, ptr, desc->buf_size, i, desc->buf_mask, i);
      ptr += desc->buf_size;
	  }
	  io_uring_buf_ring_advance(desc->br, desc->buf_count);
  }
  machine->buffer_ring_count++;
  return bg_id;
}

inline VALUE um_get_string_from_buffer_ring(struct um *machine, int bgid, __s32 result, __u32 flags) {
  if (!result) return Qnil;

  unsigned buf_idx = flags >> IORING_CQE_BUFFER_SHIFT;
  struct buf_ring_descriptor *desc = machine->buffer_rings + bgid;
  char *src = desc->buf_base + desc->buf_size * buf_idx;
  // TODO: add support for UTF8
  // buf = rd->utf8_encoding ? rb_utf8_str_new(src, cqe->res) : rb_str_new(src, cqe->res);
  VALUE buf = rb_str_new(src, result);

  // add buffer back to buffer ring
  io_uring_buf_ring_add(
    desc->br, src, desc->buf_size, buf_idx, desc->buf_mask, 0
  );
  io_uring_buf_ring_advance(desc->br, 1);

  RB_GC_GUARD(buf);
  return buf;
}

inline void um_add_strings_to_buffer_ring(struct um *machine, int bgid, VALUE strings) {
  static ID ID_to_s = 0;

  struct buf_ring_descriptor *desc = machine->buffer_rings + bgid;
  ulong count = RARRAY_LEN(strings);
  VALUE str = Qnil;
  VALUE converted = Qnil;

  for (ulong i = 0; i < count; i++) {
    str = rb_ary_entry(strings, i);
    if (TYPE(str) != T_STRING) {
      if (!ID_to_s) ID_to_s = rb_intern("to_s");
      if (NIL_P(converted)) converted = rb_ary_new();
      str = rb_funcall(str, ID_to_s, 0);
      rb_ary_push(converted, str);
    }
    io_uring_buf_ring_add(desc->br, RSTRING_PTR(str), RSTRING_LEN(str), i, desc->buf_mask, i);
  }
  RB_GC_GUARD(converted);
  io_uring_buf_ring_advance(desc->br, count);
}

inline void um_raise_internal_error(const char *msg) {
  rb_raise(eUMError, "UringMachine error: %s", msg);
}
