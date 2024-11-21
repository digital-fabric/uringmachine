#ifndef UM_SSL_H
#define UM_SSL_H

#include <ruby.h>
// #include <openssl/bio.h>
#include <openssl/ssl.h>
// #include <openssl/dh.h>
// #include <openssl/err.h>
// #include <openssl/x509.h>

enum ssl_role {
  ROLE_SERVER,
  ROLE_CLIENT
};

struct um_ssl_connection {
  VALUE self;

  enum ssl_role role;
};

#endif // UM_SSL_H
