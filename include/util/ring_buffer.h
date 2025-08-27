#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define RING_BUFFER_CAPACITY (uint64_t) (1 << 16)  // 2^18 = 262,144 Bytes

typedef struct 
{
    int16_t data[RING_BUFFER_CAPACITY];

    uint32_t    read_pos;
    uint32_t   write_pos;
    _Atomic int64_t size;

} RingBuffer;

static inline bool ring_buffer_empty(RingBuffer *rb)
{
    return (rb->write_pos == rb->read_pos);
}

static inline void reset_ring_buffer(RingBuffer *rb)
{
    rb->write_pos = 0;
    rb-> read_pos = 0;
    rb->     size = 0;
}

static inline bool ring_buffer_write(RingBuffer *rb, int16_t sample) 
{
    uint32_t next = (rb->write_pos + 1) % RING_BUFFER_CAPACITY;
    
    if (next == rb->read_pos)
        return false;

    rb->size++;
    rb->data[rb->write_pos] = sample;
    rb->          write_pos =   next;

    return true;
}

static inline bool ring_buffer_read(RingBuffer *rb, int16_t *sample) 
{
    if (ring_buffer_empty(rb)) 
        return false;
    
    rb->size--;
    *sample = rb->data[rb->read_pos];
    rb->read_pos = (rb->read_pos + 1) % RING_BUFFER_CAPACITY;

    return true;
}

#endif
