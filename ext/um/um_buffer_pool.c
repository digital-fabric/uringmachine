#include "um.h"

#define BGID2IDX(bgid) (bgid & 0XFFFF)
#define IDX2BGID(idx) (0x10000 | idx)

void um_buffer_pool_setup(struct um *machine) {
  struct um_buffer_pool *pool = &machine->buffer_pool;
  pool->group_count = 0;
  pool->buffer_freelist = NULL;
}

inline void buffer_free(struct um_buffer *buffer) {
  // TODO: free each batch?
}

inline void buffer_group_free(struct um_buffer_group *group) {
  // TODO: release buffers?
  free(group);
}

void um_buffer_pool_teardown(struct um *machine) {
  struct um_buffer_pool *pool = &machine->buffer_pool;
  int group_count = pool->group_count;
  for (int idx = 0; idx < group_count; idx++)
    buffer_group_free(pool->groups[idx]);

  struct um_buffer *cur = pool->buffer_freelist;
  while (cur) {
    struct um_buffer *next = cur->next;
    buffer_free(cur);
    cur = next;
  }
}

struct um_buffer *buffer_alloc(struct um *machine) {
  struct um_buffer *buffer = malloc(sizeof(struct um_buffer) + BUFFER_SIZE);
  machine->metrics.buffers_allocated += 1;
  machine->metrics.buffer_space_allocated += BUFFER_SIZE;
  buffer->len = BUFFER_SIZE;
  buffer->next = NULL;
  return buffer;
}

struct um_buffer *buffer_checkout(struct um *machine) {
  struct um_buffer *buffer = machine->buffer_pool.buffer_freelist;
  if (buffer) {
    struct um_buffer *next = buffer->next;
    machine->buffer_pool.buffer_freelist = next;
    machine->metrics.buffers_free--;
  }
  else
    buffer = buffer_alloc(machine);

  buffer->pos = 0;
  buffer->next = NULL;

  return buffer;
}

inline void buffer_checkin(struct um *machine, struct um_buffer *buffer) {
  buffer->next = machine->buffer_pool.buffer_freelist;
  machine->buffer_pool.buffer_freelist = buffer;
  machine->metrics.buffers_free++;
}

inline void um_buffer_release(struct um *machine, struct um_buffer *buffer) {
  buffer->ref_count--;
  if (!buffer->ref_count) buffer_checkin(machine, buffer);
}

inline int group_find_available_bid(struct um *machine, struct um_buffer_group *group, int last) {
  for (last++; last < MAX_BUFFERS_PER_GROUP; last++) {
    if (!group->buffers[last]) return last;
  }
  return -1;
}

void group_commit_buffers(struct um_buffer_group *group, uint added) {
  io_uring_buf_ring_advance(group->br, added);
}

inline int group_is_enough_space_p(struct um_buffer_group *group) {
  return group->total_commited >= (BUFFER_SIZE * 4);
}

void group_add_buffer(struct um_buffer_group *group, struct um_buffer *buffer, int bid, int offset) {
  static int buf_mask;
  if (!buf_mask) buf_mask = io_uring_buf_ring_mask(MAX_BUFFERS_PER_GROUP);

  group->buffers[bid] = buffer;
  group->buffer_count++;
  group->total_commited += buffer->len;
  buffer->ref_count++;

  io_uring_buf_ring_add(group->br, buffer->buf, buffer->len, bid, buf_mask, offset);
}

#define ENOUGH_SPACE (BUFFER_SIZE * 4)

inline int group_ensure_buffer_space(struct um *machine, struct um_buffer_group *group) {
  if (group->total_commited >= ENOUGH_SPACE) return true;

  int bid = -1;
  int added = 0;
  while (group->total_commited < ENOUGH_SPACE) {
    bid = group_find_available_bid(machine, group, bid);
    if (bid < 0) break;

    struct um_buffer *buffer = buffer_checkout(machine);
    group_add_buffer(group, buffer, bid, added);
    added++;
    machine->metrics.buffer_space_commited += BUFFER_SIZE;
  }

  if (added) group_commit_buffers(group, added);
  return group_is_enough_space_p(group);
}

void group_setup(struct um *machine, struct um_buffer_group *group, int bgid) {
  memset(group, 0, sizeof(struct um_buffer_group));
  group->bgid = bgid;
  int ret = 0;
  group->br = io_uring_setup_buf_ring(&machine->ring, MAX_BUFFERS_PER_GROUP, bgid, IOU_PBUF_RING_INC, &ret);
  fprintf(stderr, "* group_setup br=%p\n", group->br);
  if (!group->br) rb_syserr_fail(ret, strerror(ret));

  machine->metrics.buffer_groups++;
  group_ensure_buffer_space(machine, group);
}

struct um_buffer_group *um_buffer_group_select(struct um *machine) {
  struct um_buffer_pool *pool = &machine->buffer_pool;
  for (uint idx = 0; idx < pool->group_count; idx++) {
    struct um_buffer_group *group = pool->groups[idx];
    if (group_ensure_buffer_space(machine, group)) return group;
  }

  if (pool->group_count == MAX_BUFFER_GROUPS)
    rb_raise(eUMError, "Buffer pool exhausted");

  struct um_buffer_group *group = malloc(sizeof(struct um_buffer_group));
  group_setup(machine, group, IDX2BGID(pool->group_count));
  pool->groups[pool->group_count] = group;
  pool->group_count++;
  return group;
}

inline void um_buffer_group_remove_buffer(struct um *machine, struct um_buffer_group *group, struct um_buffer *buffer, int bid) {
  group->buffer_count--;
  group->buffers[bid] = NULL;
  um_buffer_release(machine, buffer);
}

struct um_segment *um_get_op_result_segment(struct um *machine, struct um_op *op, __s32 res, __u32 flags) {
  assert(res >= 0);
  uint bid = flags >> IORING_CQE_BUFFER_SHIFT;
  struct um_buffer *buffer = op->buffer_group->buffers[bid];
  assert(buffer);

  struct um_segment *segment = NULL;
  if (res > 0) {
    segment = um_segment_alloc(machine);
    segment->ptr = buffer->buf + buffer->pos;
    segment->len = res;
    segment->buffer = buffer;
    buffer->pos += res;
    buffer->ref_count++;

    op->buffer_group->total_commited -= res;
    machine->metrics.buffer_space_commited -= res;
  }

  if (!(flags & IORING_CQE_F_BUF_MORE)) {
    int buffer_still_commited = (res == 0) && (!(flags & IORING_CQE_F_MORE)) && (buffer->pos < buffer->len);
    if (!buffer_still_commited)
      um_buffer_group_remove_buffer(machine, op->buffer_group, buffer, bid);
  }

  return segment;
}

inline void um_buffer_group_replenish(struct um *machine, struct um_buffer_group *group) {
  if (group->total_commited < BUFFER_SIZE * 2)
    group_ensure_buffer_space(machine, group);
}

inline void um_segment_checkin(struct um *machine, struct um_segment *segment) {
  um_buffer_release(machine, segment->buffer);
  um_segment_free(machine, segment);
}
