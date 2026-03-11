#include <string.h>
#include "um.h"

#define UM_SEGMENT_ALLOC_BATCH_SIZE   256

inline struct um_buffer *buffer_alloc(struct um *machine) {
  struct um_buffer *buffer = malloc(sizeof(struct um_buffer) + machine->bp_buffer_size);
  if (unlikely(!buffer)) {
    fprintf(stderr, "!ENOMEM!\n");
    exit(1);
    rb_syserr_fail(errno, strerror(errno));
  }

  machine->metrics.buffers_allocated += 1;
  machine->metrics.buffer_space_allocated += machine->bp_buffer_size;
  buffer->len = machine->bp_buffer_size;
  buffer->ref_count = 0;
  return buffer;
}

inline struct um_buffer *bp_buffer_checkout(struct um *machine) {
  struct um_buffer *buffer = machine->bp_buffer_freelist;
  if (buffer) {
    struct um_buffer *next = buffer->next;
    machine->bp_buffer_freelist = next;
    machine->metrics.buffers_free--;
  }
  else
    buffer = buffer_alloc(machine);

  buffer->ref_count++;
  buffer->pos = 0;
  buffer->next = NULL;

  return buffer;
}

inline void buffer_free(struct um *machine, struct um_buffer *buffer) {
  if (unlikely(!machine->ring_initialized || buffer->len != machine->bp_buffer_size)) {
    // The machine is being shut down or working buffer size has changed, so the
    // buffer can be freed.
    machine->metrics.buffers_allocated -= 1;
    machine->metrics.buffer_space_allocated -= buffer->len;
    free(buffer);
  }
  else {
    // otherwise, keep it on the freelist
    buffer->next = machine->bp_buffer_freelist;
    machine->bp_buffer_freelist = buffer;
    machine->metrics.buffers_free++;
  }
}

inline void bp_buffer_checkin(struct um *machine, struct um_buffer *buffer) {
  assert(buffer->ref_count > 0);
  buffer->ref_count--;
  if (!buffer->ref_count) buffer_free(machine, buffer);
}

inline void bp_discard_buffer_freelist(struct um *machine) {
  while (machine->bp_buffer_freelist) {
    struct um_buffer *buffer = machine->bp_buffer_freelist;
    struct um_buffer *next = buffer->next;

    machine->metrics.buffers_allocated -= 1;
    machine->metrics.buffer_space_allocated -= buffer->len;

    free(buffer);
    machine->bp_buffer_freelist = next;
  }
}

inline void bp_setup(struct um *machine) {
  int ret;
  machine->bp_br = io_uring_setup_buf_ring(&machine->ring, BP_BR_ENTRIES, BP_BGID, IOU_PBUF_RING_INC, &ret);
  if (unlikely(!machine->bp_br)) rb_syserr_fail(ret, strerror(ret));

  machine->bp_buffer_size = BP_INITIAL_BUFFER_SIZE;
  machine->bp_commit_threshold = BP_INITIAL_COMMIT_THRESHOLD;
  machine->bp_commited_buffers = malloc(sizeof(struct um_buffer) * BP_BR_ENTRIES);
  memset(machine->bp_commited_buffers, 0, sizeof(struct um_buffer) * BP_BR_ENTRIES);
  memset(machine->bp_avail_bid_bitmap, 0xFF, sizeof(machine->bp_avail_bid_bitmap));
  
  machine->bp_buffer_freelist = NULL;
  machine->bp_buffer_count = 0;
  machine->bp_total_commited = 0;
}

inline void bp_teardown(struct um *machine) {
  bp_discard_buffer_freelist(machine);
  for (int i = 0; i < BP_BR_ENTRIES; i++) {
    struct um_buffer *buffer = machine->bp_commited_buffers[i];
    if (buffer) bp_buffer_checkin(machine, buffer);
  }
  free(machine->bp_commited_buffers);
}

inline int bitmap_find_first_set_bit(uint64_t *bitmap, size_t word_count) {
  uint word = 0;
  for (; word < word_count && !bitmap[word]; word++)
  if (word == word_count) return -1;
  
  int bit = ffsll(bitmap[word]);
  return (word * 64 + bit - 1);
}

inline void bitmap_set(uint64_t *bitmap, int idx) {
  uint word = idx / 64;
  uint bit = idx % 64;
  bitmap[word] |= (1U << bit);
}

inline void bitmap_unset(uint64_t *bitmap, int idx) {
  uint word = idx / 64;
  uint bit = idx % 64;
  bitmap[word] &= ~(1U << bit);
}

inline int get_available_bid(struct um *machine) {
  return bitmap_find_first_set_bit(machine->bp_avail_bid_bitmap, BP_AVAIL_BID_BITMAP_WORDS);
}

// Finds an available bid, checks out a buffer and commits it for kernel usage.
// Returns true if successful.
static inline int commit_buffer(struct um *machine, int added) {
  static int buf_mask;
  if (!buf_mask) buf_mask = io_uring_buf_ring_mask(BP_BR_ENTRIES);

  int bid = get_available_bid(machine);
  if (bid < 0) return false;

  bitmap_unset(machine->bp_avail_bid_bitmap, bid);
  struct um_buffer *buffer = bp_buffer_checkout(machine);
  assert(buffer->ref_count == 0);
  buffer->ref_count = 1;
  
  machine->bp_buffer_count++;
  machine->bp_commited_buffers[bid] = buffer;
  machine->bp_total_commited += buffer->len;
  machine->metrics.buffer_space_commited += buffer->len;
    
  io_uring_buf_ring_add(machine->bp_br, buffer->buf, buffer->len, bid, buf_mask, added);
  return true;
}

// Removes buffer from bid slot, the buffer having already been entirely
// consumed by the kernel.
inline void uncommit_buffer(struct um *machine, struct um_op *op, struct um_buffer *buffer, int bid) {
  machine->bp_buffer_count--;
  machine->bp_commited_buffers[bid] = NULL;
  bitmap_set(machine->bp_avail_bid_bitmap, bid);
  bp_buffer_checkin(machine, buffer);
}

inline struct um_buffer *get_buffer(struct um *machine, int bid) {
  struct um_buffer *buffer = machine->bp_commited_buffers[bid];
  assert(buffer);
  return buffer;
}

inline int should_commit_more_p(struct um *machine) {
  return (machine->bp_buffer_count < BP_BR_ENTRIES) && 
         (machine->bp_total_commited < machine->bp_commit_threshold);
}

inline void bp_ensure_commit_level(struct um *machine) {
  int added = 0;
  while (should_commit_more_p(machine)) {
    if (likely(commit_buffer(machine, added))) added++;
  }
  if (added) io_uring_buf_ring_advance(machine->bp_br, added);

  // if we get to this point, there's nothing more we can do because we used up
  // all buffer ring entries. We need to wait for the kernel to consume buffers
  // in order to put in more. When a ENOBUFS error is received,
  // bp_handle_enobufs is called and among other things increases the buffer
  // size.
}

inline void bp_handle_enobufs(struct um *machine) {
  if (unlikely(machine->bp_commit_threshold >= BP_MAX_COMMIT_THRESHOLD))
    rb_raise(eUMError, "Buffer starvation");

  machine->bp_commit_threshold *= 2;
  while (machine->bp_buffer_size < machine->bp_commit_threshold / 4)
    machine->bp_buffer_size *= 2;
  bp_discard_buffer_freelist(machine);
}

inline struct um_segment *um_segment_alloc(struct um *machine) {
  if (machine->segment_freelist) {
    struct um_segment *segment = machine->segment_freelist;
    machine->segment_freelist = segment->next;
    machine->metrics.segments_free--;
    segment->next = NULL;
    return segment;
  }

  struct um_segment *batch = malloc(sizeof(struct um_segment) * UM_SEGMENT_ALLOC_BATCH_SIZE);
  memset(batch, 0, sizeof(struct um_segment) * UM_SEGMENT_ALLOC_BATCH_SIZE);
  for (int i = 1; i < (UM_SEGMENT_ALLOC_BATCH_SIZE - 1); i++) {
    batch[i].next = &batch[i + 1];
  }
  machine->segment_freelist = batch + 1;
  machine->metrics.segments_free += (UM_SEGMENT_ALLOC_BATCH_SIZE - 1);
  return batch;
}

inline void um_segment_free(struct um *machine, struct um_segment *segment) {
  segment->next = machine->segment_freelist;
  machine->segment_freelist = segment;
  machine->metrics.segments_free++;
}

inline struct um_segment *bp_buffer_consume(struct um *machine, struct um_buffer *buffer, size_t len) {
  struct um_segment *segment = um_segment_alloc(machine);
  segment->ptr = buffer->buf + buffer->pos;
  segment->len = len;
  segment->buffer = buffer;
  buffer->pos += len;
  buffer->ref_count++;
  return segment;
}

struct um_segment *bp_get_op_result_segment(struct um *machine, struct um_op *op, __s32 res, __u32 flags) {
  assert(res >= 0);
  if (!res) return NULL;

  uint bid = flags >> IORING_CQE_BUFFER_SHIFT;
  struct um_buffer *buffer = get_buffer(machine, bid);

  struct um_segment *segment = bp_buffer_consume(machine, buffer, res);
  machine->bp_total_commited -= res;
  machine->metrics.buffer_space_commited -= res;
  bp_ensure_commit_level(machine);

  if (!(flags & IORING_CQE_F_BUF_MORE))
    uncommit_buffer(machine, op, buffer, bid);

  return segment;
}

inline void um_segment_checkin(struct um *machine, struct um_segment *segment) {
  bp_buffer_checkin(machine, segment->buffer);
  um_segment_free(machine, segment);
}
