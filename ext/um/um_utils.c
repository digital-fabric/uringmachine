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

inline double um_get_time_cpu(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return -1.0;

  return um_timestamp_to_double(ts.tv_sec, ts.tv_nsec);
}

inline double um_get_time_monotonic(void) {
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
    char *base;
    size_t size;
    rb_io_buffer_get_bytes_for_writing(buffer, (void *)&base, &size); // writing *to* buffer
    if (len == -1) len = size;
    if (ofs < 0) ofs = size + ofs + 1;
    size_t new_size = len + (size_t)ofs;

    if (size < new_size) {
      rb_io_buffer_resize(buffer, new_size);
      rb_io_buffer_get_bytes_for_writing(buffer, (void *)&base, &size);
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

inline void um_update_read_buffer(VALUE buffer, ssize_t buffer_offset, __s32 result) {
  if (!result) return;

  adjust_read_buffer_len(buffer, result, buffer_offset);
}

// returns false if buffer is invalid and raise_on_bad_buffer is false
inline int um_get_buffer_bytes_for_writing(VALUE buffer, const void **base, size_t *size, int raise_on_bad_buffer) {
  if (TYPE(buffer) == T_STRING) {
    *base = RSTRING_PTR(buffer);
    *size = RSTRING_LEN(buffer);
  }
  else if (IO_BUFFER_P(buffer))
    rb_io_buffer_get_bytes_for_reading(buffer, base, size); // reading *from* buffer
  else {
    if (raise_on_bad_buffer)
      um_raise_internal_error("Invalid buffer provided");
    else
      return false;
  }
  return true;
}

inline void um_raise_internal_error(const char *msg) {
  rb_raise(eUMError, "UringMachine error: %s", msg);
}

inline struct iovec *um_alloc_iovecs_for_writing(int argc, VALUE *argv, size_t *total_len) {
  struct iovec *iovecs = malloc(sizeof(struct iovec) * argc);
  size_t len = 0;

  for (int i = 0; i < argc; i++) {
    int ok = um_get_buffer_bytes_for_writing(argv[i], (const void **)&iovecs[i].iov_base, &iovecs[i].iov_len, false);
    if (unlikely(!ok)) {
      free(iovecs);
      um_raise_internal_error("Invalid buffer provided");
    }
    len += iovecs[i].iov_len;
  }
  if (total_len) *total_len = len;
  return iovecs;
}

inline void um_advance_iovecs_for_writing(struct iovec **ptr, int *len, size_t adv) {
  while (adv) {
    if (adv < (*ptr)->iov_len) {
      (*ptr)->iov_base = (char *)(*ptr)->iov_base + adv;
      (*ptr)->iov_len -= adv;
      return;
    }
    else {
      adv -= (*ptr)->iov_len;
      (*ptr)++;
      (*len)--;
    }
  }
}
