#include "um.h"

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

inline void um_raise_on_system_error(int result) {
  if (result < 0) rb_syserr_fail(-result, strerror(-result));
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

inline void um_update_read_buffer(struct um *machine, VALUE buffer, int buffer_offset, int result, int flags) {
  if (flags & IORING_CQE_F_BUFFER) {
    rb_raise(rb_eRuntimeError, "TODO: implement reading from buffer ring");
    // update_read_buffer_from_buffer_ring(iour, ctx, cqe);
    return;
  }

  if (!result) return;

  adjust_read_buffer_len(buffer, result, buffer_offset);
}

