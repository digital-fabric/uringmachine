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

VALUE um_raise_exception(VALUE e) {
  static ID ID_raise = 0;
  if (!ID_raise) ID_raise = rb_intern("raise");

  return rb_funcall(rb_mKernel, ID_raise, 1, e);
}
