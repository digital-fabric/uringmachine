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
  static ID ID_raise_exception = 0;
  
  if (!ID_raise_exception) ID_raise_exception = rb_intern("raise_exception");

  return rb_funcall(cUM, ID_raise_exception, 1, e);
}
