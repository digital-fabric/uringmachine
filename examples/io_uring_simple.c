#include <liburing.h>

struct io_uring ring;
struct io_uring_sqe *sqe;
struct io_uring_cqe *cqe;
int ret;
char buf[100];

ret = io_uring_queue_init(8, &ring, 0);
sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, STDIN_FILENO, buf, 100, 0);
sqe->user_data = 42;
io_uring_submit(&ring);

...

ret = io_uring_wait_cqe(&ring, &cqe);
if (!ret) {
  if (cqe->user_data == 42) {
    int len = cqe->res;
    printf("Got: %d\n", len);
  }
  io_uring_cqe_seen(&ring, cqe);
}
