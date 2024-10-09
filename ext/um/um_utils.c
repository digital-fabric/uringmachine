#include "um.h"
#include <sys/mman.h>
#include <stdlib.h>

inline struct __kernel_timespec um_double_to_timespec(double value) {
  double integral;
  double fraction = modf(value, &integral);
  struct __kernel_timespec ts;
  ts.tv_sec = integral;
  ts.tv_nsec = floor(fraction * 1000000000);
  return ts;
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

inline void * um_prepare_read_buffer(VALUE buffer, unsigned len, int ofs) {
  unsigned current_len = RSTRING_LEN(buffer);
  if (ofs < 0) ofs = current_len + ofs + 1;
  unsigned new_len = len + (unsigned)ofs;

  if (current_len < new_len)
    rb_str_modify_expand(buffer, new_len);
  else
    rb_str_modify(buffer);
  return RSTRING_PTR(buffer) + ofs;
}

static inline void adjust_read_buffer_len(VALUE buffer, int result, int ofs) {
  rb_str_modify(buffer);
  unsigned len = result > 0 ? (unsigned)result : 0;
  unsigned current_len = RSTRING_LEN(buffer);
  if (ofs < 0) ofs = current_len + ofs + 1;
  rb_str_set_len(buffer, len + (unsigned)ofs);
}

inline void um_update_read_buffer(struct um *machine, VALUE buffer, int buffer_offset, __s32 result, __u32 flags) {
  if (!result) return;

  adjust_read_buffer_len(buffer, result, buffer_offset);
}

int um_setup_buffer_ring(struct um *machine, unsigned size, unsigned count) {
  if (machine->buffer_ring_count == BUFFER_RING_MAX_COUNT)
    rb_raise(rb_eRuntimeError, "Cannot setup more than BUFFER_RING_MAX_COUNT buffer rings");

  struct buf_ring_descriptor *desc = machine->buffer_rings + machine->buffer_ring_count;
  desc->buf_count = count;
  desc->buf_size = size;

  desc->br_size = sizeof(struct io_uring_buf) * desc->buf_count;
	void *mapped = mmap(
    NULL, desc->br_size, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, 0, 0
  );
  if (mapped == MAP_FAILED)
    rb_raise(rb_eRuntimeError, "Failed to allocate buffer ring");

  desc->br = (struct io_uring_buf_ring *)mapped;
  io_uring_buf_ring_init(desc->br);

  unsigned bg_id = machine->buffer_ring_count;
  int ret;
  desc->br = io_uring_setup_buf_ring(&machine->ring, count, bg_id, 0, &ret);
  if (!desc->br) {
    munmap(desc->br, desc->br_size);
    rb_syserr_fail(ret, strerror(ret));
  }

  if (posix_memalign(&desc->buf_base, 4096, desc->buf_count * desc->buf_size)) {
    io_uring_free_buf_ring(&machine->ring, desc->br, desc->buf_count, bg_id);
    rb_raise(rb_eRuntimeError, "Failed to allocate buffers");
  }

  desc->buf_mask = io_uring_buf_ring_mask(desc->buf_count);
  void *ptr = desc->buf_base;
  for (unsigned i = 0; i < desc->buf_count; i++) {
		io_uring_buf_ring_add(desc->br, ptr, desc->buf_size, i, desc->buf_mask, i);
    ptr += desc->buf_size;
	}
	io_uring_buf_ring_advance(desc->br, desc->buf_count);
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
