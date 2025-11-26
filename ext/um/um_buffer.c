#include "um.h"

inline long buffer_size(long len) {
  len--;
  len |= len >> 1;
  len |= len >> 2;
  len |= len >> 4;
  len |= len >> 8;
  len |= len >> 16;
  len++;
  return (len > 4096) ? len : 4096;
}

inline struct um_buffer *um_buffer_checkout(struct um *machine, int len) {
  struct um_buffer *buffer = machine->buffer_freelist;
  if (buffer)
    machine->buffer_freelist = buffer->next;
  else {
    buffer = malloc(sizeof(struct um_buffer));
    memset(buffer, 0, sizeof(struct um_buffer));
  }

  if (buffer->len < len) {
    if (buffer->ptr) {
      free(buffer->ptr);
      buffer->ptr = NULL;
    }

    buffer->len = buffer_size(len);
    if (posix_memalign(&buffer->ptr, 4096, buffer->len))
      um_raise_internal_error("Failed to allocate buffer");
  }
  return buffer;
}

inline void um_buffer_checkin(struct um *machine, struct um_buffer *buffer) {
  buffer->next = machine->buffer_freelist;
  machine->buffer_freelist = buffer;
}

inline void um_free_buffer_linked_list(struct um *machine) {
  struct um_buffer *buffer = machine->buffer_freelist;
  while (buffer) {
    struct um_buffer *next = buffer->next;
    if (buffer->ptr) free(buffer->ptr);
    free(buffer);
    buffer = next;
  }
}
