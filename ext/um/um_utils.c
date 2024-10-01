#include "um.h"

inline struct __kernel_timespec um_double_to_timespec(double value) {
  double integral;
  double fraction = modf(value, &integral);
  struct __kernel_timespec ts;
  ts.tv_sec = integral;
  ts.tv_nsec = floor(fraction * 1000000000);
  return ts;
}

int um_value_is_exception_p(VALUE v) {
  // TODO: check if v is an exception
  return 0;
}

VALUE um_raise_exception(VALUE v) {
  // TODO: raise exception
  return Qnil;
}
