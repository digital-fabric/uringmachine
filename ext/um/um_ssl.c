#include "um.h"
#include <openssl/ssl.h>
#include <openssl/bio.h>

#define IDX_BIO_DATA_MACHINE  1
#define IDX_BIO_DATA_FD       2

static int um_bio_read(BIO *bio, char *buf, int blen)
{
  struct um *machine = (struct um *)BIO_get_ex_data(bio, IDX_BIO_DATA_MACHINE);
  long fd = (long)BIO_get_ex_data(bio, IDX_BIO_DATA_FD);
  return (int)um_read_raw(machine, fd, buf, blen);
}

static int um_bio_write(BIO *bio, const char *buf, int blen)
{
  struct um *machine = (struct um *)BIO_get_ex_data(bio, IDX_BIO_DATA_MACHINE);
  long fd = (long)BIO_get_ex_data(bio, IDX_BIO_DATA_FD);
  return (int)um_write_raw(machine, fd, buf, blen);
}

static long um_bio_ctrl(BIO *bio, int cmd, long num, void *ptr)
{
  switch(cmd) {
    case BIO_CTRL_GET_CLOSE:
      return (long)BIO_get_shutdown(bio);
    case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(bio, (int)num);
      return 1;
    case BIO_CTRL_FLUSH:
      // we don't buffer writes, so noop
      return 1;
    default:
      return 0;
  }
}

BIO_METHOD *um_ssl_create_bio_method(void)
{
  BIO_METHOD *m = BIO_meth_new(BIO_TYPE_MEM, "UringMachine BIO");
  if(m) {
    BIO_meth_set_write(m, &um_bio_write);
    BIO_meth_set_read(m, &um_bio_read);
    BIO_meth_set_ctrl(m, &um_bio_ctrl);
  }
  else
    rb_raise(eUMError, "Failed to set SSL BIO");
  return m;
}


static BIO_METHOD *um_bio_method = NULL;
static ID id_ivar_um_bio;

void um_ssl_set_bio(struct um *machine, VALUE ssl_obj)
{
  if (!um_bio_method) {
    um_bio_method = um_ssl_create_bio_method();
    id_ivar_um_bio = rb_intern_const("@__um_bio__");
  }

  long fd = NUM2LONG(rb_funcall(ssl_obj, rb_intern_const("fileno"), 0));
  rb_ivar_set(ssl_obj, id_ivar_um_bio, Qtrue);

  BIO *bio = BIO_new(um_bio_method);
  if(!bio)
    rb_raise(eUMError, "Failed to create custom BIO");

  int ret = BIO_set_ex_data(bio, IDX_BIO_DATA_MACHINE, (void *)machine);
  if (!ret)
    rb_raise(eUMError, "Failed to set BIO metadata");

  ret = BIO_set_ex_data(bio, IDX_BIO_DATA_FD, (void *)fd);
  if (!ret)
    rb_raise(eUMError, "Failed to set BIO metadata");

  SSL *ssl = RTYPEDDATA_GET_DATA(ssl_obj);
  BIO_up_ref(bio);
  SSL_set0_rbio(ssl, bio);
  SSL_set0_wbio(ssl, bio);
}

int um_ssl_read(struct um *machine, VALUE ssl_obj, VALUE buf, int maxlen) {
  SSL *ssl = RTYPEDDATA_GET_DATA(ssl_obj);
  void *ptr = um_prepare_read_buffer(buf, maxlen, 0);
  int ret = SSL_read(ssl, ptr, maxlen);
  if (ret > 0) {
    um_update_read_buffer(machine, buf, 0, ret, 0);
  }
  else {
    rb_raise(eUMError, "Failed to read");
  }
  return ret;
}

int um_ssl_write(struct um *machine, VALUE ssl_obj, VALUE buf, int len) {
  SSL *ssl = RTYPEDDATA_GET_DATA(ssl_obj);
  const void *base;
  size_t size;
  um_get_buffer_bytes_for_writing(buf, &base, &size);
  if ((len == (int)-1) || (len > (int)size)) len = (int)size;
  if (unlikely(!len)) return INT2NUM(0);

  int ret = SSL_write(ssl, base, len);
  if (ret <= 0) {
    rb_raise(eUMError, "Failed to read");
  }
  return ret;
}
