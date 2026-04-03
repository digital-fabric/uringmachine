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

sqe = io_uring_get_sqe(&ring);
io_uring_prep_cancel(sqe, 42, IORING_ASYNC_CANCEL_USERDATA);
sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
io_uring_submit(&ring);

if (!io_uring_wait_cqe(&ring, &cqe)) {
  if (cqe->user_data == 42) {
    if (cqe->res == -ECANCELED)
      printf("Cancelled!\n");
    else {
      int len = cqe->res;
      printf("Got %d bytes\n", len);
    }
  }
  io_uring_cqe_seen(&ring, cqe);
}
