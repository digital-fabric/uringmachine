/*
  Adopted from:
    https://github.com/puma/puma/blob/master/ext/puma_http11/mini_ssl.c
  
  License (BSD-3):
    https://github.com/puma/puma/blob/master/LICENSE
*/

#include "um.h"
#include "um_ssl.h"
// #include <openssl/bio.h>
#include <openssl/ssl.h>
// #include <openssl/dh.h>
// #include <openssl/err.h>
// #include <openssl/x509.h>

VALUE eSSLError;
VALUE cSSLConnection;

static void UM_SSL_Connection_mark(void *ptr) {
  // struct um_ssl_connection *connection = ptr;
  // rb_gc_mark_movable(connection->self);

  // um_op_list_mark(machine, machine->transient_head);
  // um_op_list_mark(machine, machine->runqueue_head);
}

static void UM_SSL_Connection_compact(void *ptr) {
  // struct um_ssl_connection *connection = ptr;
  // machine->self = rb_gc_location(machine->self);
  // machine->poll_fiber = rb_gc_location(machine->poll_fiber);

  // um_op_list_compact(machine, machine->transient_head);
  // um_op_list_compact(machine, machine->runqueue_head);
}

static void UM_SSL_Connection_free(void *ptr) {
  // um_ssl_connection_teardown((struct um_ssl_connection *)ptr);
  free(ptr);
}

static size_t UM_SSL_Connection_size(const void *ptr) {
  return sizeof(struct um_ssl_connection);
}

static const rb_data_type_t UM_SSL_Connection_type = {
    "UringMachine::SSL::Connection",
    {UM_SSL_Connection_mark, UM_SSL_Connection_free, UM_SSL_Connection_size, UM_SSL_Connection_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static VALUE UM_SSL_Connection_allocate(VALUE klass) {
  struct um_ssl_connection *connection = ALLOC(struct um_ssl_connection);
  return TypedData_Wrap_Struct(klass, &UM_SSL_Connection_type, connection);
}

VALUE UM_SSL_Connection_initialize(VALUE self, VALUE machine, VALUE fd, VALUE ctx) {
  // struct um_ssl_connection *connection = RTYPEDDATA_DATA(self);

  return self;
}

VALUE UM_SSL_check(VALUE self) {
  return Qnil;
}

void Init_SSL(void) {
  VALUE cUM = rb_define_class("UringMachine", rb_cObject);
  VALUE cSSL = rb_define_module_under(cUM, "SSL");

  cSSLConnection = rb_define_class_under(cSSL, "Connection", rb_cObject);
  rb_define_alloc_func(cUM, UM_SSL_Connection_allocate);

  rb_define_method(cSSLConnection, "initialize", UM_SSL_Connection_initialize, 3);

  // cCtx = rb_define_class_under(cSSL, "SSLContext", rb_cObject);
  // rb_define_alloc_func(cCtx, sslctx_alloc);
  // rb_define_method(cCtx, "initialize", sslctx_initialize, 1);
  // rb_undef_method(cCtx, "initialize_copy");

  // OpenSSL Build / Runtime/Load versions

  /* Version of OpenSSL that UringMachine was compiled with */
  rb_define_const(cSSL, "OPENSSL_VERSION", rb_str_new2(OPENSSL_VERSION_TEXT));

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000
  /* Version of OpenSSL that UringMachine loaded with */
  rb_define_const(cSSL, "OPENSSL_LIBRARY_VERSION", rb_str_new2(OpenSSL_version(OPENSSL_VERSION)));
#else
  rb_define_const(cSSL, "OPENSSL_LIBRARY_VERSION", rb_str_new2(SSLeay_version(SSLEAY_VERSION)));
#endif

#if defined(OPENSSL_NO_SSL3) || defined(OPENSSL_NO_SSL3_METHOD)
  /* True if SSL3 is not available */
  rb_define_const(cSSL, "OPENSSL_NO_SSL3", Qtrue);
#else
  rb_define_const(cSSL, "OPENSSL_NO_SSL3", Qfalse);
#endif

#if defined(OPENSSL_NO_TLS1) || defined(OPENSSL_NO_TLS1_METHOD)
  /* True if TLS1 is not available */
  rb_define_const(cSSL, "OPENSSL_NO_TLS1", Qtrue);
#else
  rb_define_const(cSSL, "OPENSSL_NO_TLS1", Qfalse);
#endif

#if defined(OPENSSL_NO_TLS1_1) || defined(OPENSSL_NO_TLS1_1_METHOD)
  /* True if TLS1_1 is not available */
  rb_define_const(cSSL, "OPENSSL_NO_TLS1_1", Qtrue);
#else
  rb_define_const(cSSL, "OPENSSL_NO_TLS1_1", Qfalse);
#endif

  rb_define_singleton_method(cSSL, "check", UM_SSL_check, 0);

  eSSLError = rb_define_class_under(cSSL, "SSLError", rb_eStandardError);

  // rb_define_singleton_method(cSSLConnection, "server", engine_init_server, 1);
  // rb_define_singleton_method(cSSLConnection, "client", engine_init_client, 0);

  // rb_define_method(cSSLConnection, "inject", engine_inject, 1);
  // rb_define_method(cSSLConnection, "read",  engine_read, 0);

  // rb_define_method(cSSLConnection, "write",  engine_write, 1);
  // rb_define_method(cSSLConnection, "extract", engine_extract, 0);

  // rb_define_method(cSSLConnection, "shutdown", engine_shutdown, 0);

  // rb_define_method(cSSLConnection, "init?", engine_init, 0);

  /* @!attribute [r] peercert
   * Returns `nil` when `MiniSSL::Context#verify_mode` is set to `VERIFY_NONE`.
   * @return [String, nil] DER encoded cert
   */
  // rb_define_method(cSSLConnection, "peercert", engine_peercert, 0);

  // rb_define_method(cSSLConnection, "ssl_vers_st", engine_ssl_vers_st, 0);
}
