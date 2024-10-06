#include "um.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netdb.h>
#include <net/if.h>

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

#define DEF_CONST_INT(mod, v) rb_define_const(mod, #v, INT2NUM(v))

void um_define_net_constants(VALUE mod) {
  DEF_CONST_INT(mod, SOCK_STREAM);
  DEF_CONST_INT(mod, SOCK_DGRAM);
  DEF_CONST_INT(mod, SOCK_RAW);
  DEF_CONST_INT(mod, SOCK_RDM);
  DEF_CONST_INT(mod, SOCK_SEQPACKET);
  DEF_CONST_INT(mod, SOCK_PACKET);
  DEF_CONST_INT(mod, SOCK_CLOEXEC);

  DEF_CONST_INT(mod, AF_UNSPEC);
  DEF_CONST_INT(mod, PF_UNSPEC);
  DEF_CONST_INT(mod, AF_INET);
  DEF_CONST_INT(mod, PF_INET);
  DEF_CONST_INT(mod, AF_INET6);
  DEF_CONST_INT(mod, PF_INET6);
  DEF_CONST_INT(mod, AF_UNIX);
  DEF_CONST_INT(mod, PF_UNIX);
  DEF_CONST_INT(mod, AF_LOCAL);
  DEF_CONST_INT(mod, PF_LOCAL);
  DEF_CONST_INT(mod, AF_ROUTE);
  DEF_CONST_INT(mod, PF_ROUTE);
  DEF_CONST_INT(mod, AF_MAX);
  DEF_CONST_INT(mod, PF_MAX);

  DEF_CONST_INT(mod, MSG_OOB);
  DEF_CONST_INT(mod, MSG_PEEK);
  DEF_CONST_INT(mod, MSG_DONTROUTE);
  DEF_CONST_INT(mod, MSG_WAITALL);
  DEF_CONST_INT(mod, MSG_DONTWAIT);
  DEF_CONST_INT(mod, MSG_MORE);

  DEF_CONST_INT(mod, SOL_SOCKET);
  DEF_CONST_INT(mod, SOL_IP);

  DEF_CONST_INT(mod, IPPROTO_IP);
  DEF_CONST_INT(mod, IPPROTO_ICMP);
  DEF_CONST_INT(mod, IPPROTO_IGMP);
  DEF_CONST_INT(mod, IPPROTO_TCP);
  DEF_CONST_INT(mod, IPPROTO_EGP);
  DEF_CONST_INT(mod, IPPROTO_PUP);
  DEF_CONST_INT(mod, IPPROTO_UDP);
  DEF_CONST_INT(mod, IPPROTO_IDP);
  DEF_CONST_INT(mod, IPPROTO_IPV6);
  DEF_CONST_INT(mod, IPPROTO_NONE);
  DEF_CONST_INT(mod, IPPROTO_ROUTING);
  DEF_CONST_INT(mod, IPPROTO_RAW);
  DEF_CONST_INT(mod, IPPROTO_MAX);

  DEF_CONST_INT(mod, INADDR_ANY);
  DEF_CONST_INT(mod, INADDR_BROADCAST);
  DEF_CONST_INT(mod, INADDR_LOOPBACK);
  DEF_CONST_INT(mod, INADDR_NONE);

  DEF_CONST_INT(mod, IP_OPTIONS);
  DEF_CONST_INT(mod, IP_HDRINCL);
  DEF_CONST_INT(mod, IP_TOS);
  DEF_CONST_INT(mod, IP_TTL);
  DEF_CONST_INT(mod, IP_RECVOPTS);
  DEF_CONST_INT(mod, IP_MINTTL);
  DEF_CONST_INT(mod, IP_RECVTTL);

  DEF_CONST_INT(mod, SO_DEBUG);
  DEF_CONST_INT(mod, SO_REUSEADDR);
  DEF_CONST_INT(mod, SO_REUSEPORT);
  DEF_CONST_INT(mod, SO_TYPE);
  DEF_CONST_INT(mod, SO_ERROR);
  DEF_CONST_INT(mod, SO_DONTROUTE);
  DEF_CONST_INT(mod, SO_BROADCAST);
  DEF_CONST_INT(mod, SO_SNDBUF);
  DEF_CONST_INT(mod, SO_RCVBUF);
  DEF_CONST_INT(mod, SO_SNDBUFFORCE);
  DEF_CONST_INT(mod, SO_RCVBUFFORCE);
  DEF_CONST_INT(mod, SO_KEEPALIVE);
  DEF_CONST_INT(mod, SO_OOBINLINE);
  DEF_CONST_INT(mod, SO_PRIORITY);
  DEF_CONST_INT(mod, SO_LINGER);
  DEF_CONST_INT(mod, SO_PASSCRED);
  DEF_CONST_INT(mod, SO_PEERCRED);
  DEF_CONST_INT(mod, SO_RCVLOWAT);
  DEF_CONST_INT(mod, SO_SNDLOWAT);
  DEF_CONST_INT(mod, SO_RCVTIMEO);
  DEF_CONST_INT(mod, SO_SNDTIMEO);
  DEF_CONST_INT(mod, SO_ACCEPTCONN);
  DEF_CONST_INT(mod, SO_PEERNAME);
  DEF_CONST_INT(mod, SO_TIMESTAMP);
  DEF_CONST_INT(mod, SO_MARK);
  DEF_CONST_INT(mod, SO_PROTOCOL);
  DEF_CONST_INT(mod, SO_DOMAIN);
  DEF_CONST_INT(mod, SO_PEEK_OFF);
  DEF_CONST_INT(mod, SO_BUSY_POLL);

  DEF_CONST_INT(mod, TCP_NODELAY);
  DEF_CONST_INT(mod, TCP_MAXSEG);
  DEF_CONST_INT(mod, TCP_CORK);
  DEF_CONST_INT(mod, TCP_DEFER_ACCEPT);
  DEF_CONST_INT(mod, TCP_INFO);
  DEF_CONST_INT(mod, TCP_KEEPCNT);
  DEF_CONST_INT(mod, TCP_KEEPIDLE);
  DEF_CONST_INT(mod, TCP_KEEPINTVL);
  DEF_CONST_INT(mod, TCP_LINGER2);
  DEF_CONST_INT(mod, TCP_MD5SIG);
  DEF_CONST_INT(mod, TCP_QUICKACK);
  DEF_CONST_INT(mod, TCP_SYNCNT);
  DEF_CONST_INT(mod, TCP_WINDOW_CLAMP);
  DEF_CONST_INT(mod, TCP_FASTOPEN);
  DEF_CONST_INT(mod, TCP_CONGESTION);
  DEF_CONST_INT(mod, TCP_COOKIE_TRANSACTIONS);
  DEF_CONST_INT(mod, TCP_QUEUE_SEQ);
  DEF_CONST_INT(mod, TCP_REPAIR);
  DEF_CONST_INT(mod, TCP_REPAIR_OPTIONS);
  DEF_CONST_INT(mod, TCP_REPAIR_QUEUE);
  DEF_CONST_INT(mod, TCP_THIN_LINEAR_TIMEOUTS);
  DEF_CONST_INT(mod, TCP_TIMESTAMP);
  DEF_CONST_INT(mod, TCP_USER_TIMEOUT);
  
  DEF_CONST_INT(mod, UDP_CORK);

  DEF_CONST_INT(mod, AI_PASSIVE);
  DEF_CONST_INT(mod, AI_CANONNAME);
  DEF_CONST_INT(mod, AI_NUMERICHOST);
  DEF_CONST_INT(mod, AI_NUMERICSERV);
  DEF_CONST_INT(mod, AI_ALL);
  DEF_CONST_INT(mod, AI_ADDRCONFIG);
  DEF_CONST_INT(mod, AI_V4MAPPED);
  
  DEF_CONST_INT(mod, NI_MAXHOST);
  DEF_CONST_INT(mod, NI_MAXSERV);
  DEF_CONST_INT(mod, NI_NOFQDN);
  DEF_CONST_INT(mod, NI_NUMERICHOST);
  DEF_CONST_INT(mod, NI_NAMEREQD);
  DEF_CONST_INT(mod, NI_NUMERICSERV);
  DEF_CONST_INT(mod, NI_DGRAM);
  
  DEF_CONST_INT(mod, SHUT_RD);
  DEF_CONST_INT(mod, SHUT_WR);
  DEF_CONST_INT(mod, SHUT_RDWR);

  DEF_CONST_INT(mod, IPV6_JOIN_GROUP);
  DEF_CONST_INT(mod, IPV6_LEAVE_GROUP);
  DEF_CONST_INT(mod, IPV6_MULTICAST_HOPS);
  DEF_CONST_INT(mod, IPV6_MULTICAST_IF);
  DEF_CONST_INT(mod, IPV6_MULTICAST_LOOP);
  DEF_CONST_INT(mod, IPV6_UNICAST_HOPS);
  DEF_CONST_INT(mod, IPV6_V6ONLY);
  DEF_CONST_INT(mod, IPV6_CHECKSUM);
  DEF_CONST_INT(mod, IPV6_DONTFRAG);
  DEF_CONST_INT(mod, IPV6_DSTOPTS);
  DEF_CONST_INT(mod, IPV6_HOPLIMIT);
  DEF_CONST_INT(mod, IPV6_HOPOPTS);
  DEF_CONST_INT(mod, IPV6_NEXTHOP);
  DEF_CONST_INT(mod, IPV6_PATHMTU);
  DEF_CONST_INT(mod, IPV6_PKTINFO);
  DEF_CONST_INT(mod, IPV6_RECVDSTOPTS);
  DEF_CONST_INT(mod, IPV6_RECVHOPLIMIT);
  DEF_CONST_INT(mod, IPV6_RECVHOPOPTS);
  DEF_CONST_INT(mod, IPV6_RECVPKTINFO);
  DEF_CONST_INT(mod, IPV6_RECVRTHDR);
  DEF_CONST_INT(mod, IPV6_RECVTCLASS);
  DEF_CONST_INT(mod, IPV6_RTHDR);
  DEF_CONST_INT(mod, IPV6_RTHDRDSTOPTS);
  DEF_CONST_INT(mod, IPV6_RTHDR_TYPE_0);
  DEF_CONST_INT(mod, IPV6_RECVPATHMTU);
  DEF_CONST_INT(mod, IPV6_TCLASS);

  DEF_CONST_INT(mod, INET_ADDRSTRLEN);
  DEF_CONST_INT(mod, INET6_ADDRSTRLEN);

  DEF_CONST_INT(mod, IF_NAMESIZE);

  DEF_CONST_INT(mod, SOMAXCONN);
}