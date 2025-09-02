#ifndef CIRCULAR_QUEUE_H
#define CIRCULAR_QUEUE_H

typedef enum
{
    PIXEL,
    OBJECT

} QueueOptions;

typedef struct
{
    uint8_t        color;
    uint8_t  dmg_palette;
    uint8_t  cgb_palette;
    bool        priority;

} GbcPixel;

typedef struct
{
    uint16_t oam_address; // Object start in memory.

    uint8_t            y; // Byte-1
    uint8_t            x; // Byte-2
    uint8_t   tile_index; // Byte-3
    // Byte-4
    bool        priority; // Bit-7
    bool          y_flip; // Bit-6 
    bool          x_flip; // Bit-5
    uint8_t  dmg_palette; // Bit-4
    uint8_t         bank; // Bit-3
    uint8_t  cgb_palette; // Bits 2-0

} OamObject; 

typedef struct
{
    void      **items;

    int         front;
    int          rear;
    int          size;
    int      capacity;

    QueueOptions type;

} Queue;

/* Core Queue Functionality */ 

bool is_full(Queue *queue);

bool is_empty(Queue *queue);

void enqueue_pixel(Queue *queue, GbcPixel *pixel);

void enqueue_object(Queue *queue, OamObject *obj);

void *peek(Queue *queue);

void *dequeue(Queue *queue);

void reset_queue(Queue *queue);

void sort_oam_by_xpos(Queue *queue);

uint8_t queue_size(Queue *queue);

/* Item Generation */

GbcPixel *generate_pixel();

OamObject *generate_object();

/* Queue Initialization */

void print_queue(Queue *queue);

Queue *init_queue(uint8_t capacity, QueueOptions queue_type);

void tidy_queue(Queue *queue); 

#endif
