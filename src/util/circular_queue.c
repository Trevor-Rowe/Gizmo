#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/circular_queue.h"

/* CIRCULAR QUEUE IMPLEMENTATION FOR PIXEL FETCHER */

bool is_full(Queue *queue)
{
    return queue->size == queue->capacity;
}

bool is_empty(Queue *queue)
{
    return queue->size == 0;
}

void enqueue_pixel(Queue *queue, GbcPixel *pixel)
{
    if (is_full(queue)) return;
    // Adjust rear index to next available slot.
    queue->rear = (queue->rear + 1) % queue->capacity;
    GbcPixel *item = (GbcPixel*) queue->items[queue->rear];
    memcpy(item, pixel, sizeof(GbcPixel));
    // Move the front of queue to next available object.
    if (queue->front == -1) queue->front = 0;
    queue->size++;
}

void enqueue_object(Queue *queue, OamObject *obj)
{
    if (is_full(queue)) return;
    // Adjust rear index to next available slot.
    queue->rear = (queue->rear + 1) % queue->capacity;
    OamObject *item = (OamObject*) queue->items[queue->rear];
    memcpy(item, obj, sizeof(OamObject));
    // Move the front of queue to next available object.
    if (queue->front == -1) queue->front = 0;
    queue->size++;
}

void reset_queue(Queue *queue)
{
    queue->front    = -1;
    queue->rear     = -1;
    queue->size     =  0; 
}

void *peek(Queue *queue)
{
    if (queue->size == 0) return NULL;

    return queue->items[queue->front];
}

void *dequeue(Queue *queue)
{
    if (is_empty(queue)) return NULL;
    // Grab the front.
    void *item = queue->items[queue->front];
    // Adjust index.
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size--;
    
    return item;
}

void sort_oam_by_xpos(Queue *queue)
{
    if (queue->size < 2) return;
    // Seed index for Insertion Sort Algo.
    uint8_t index = 1;
    // Enforce assumption that these are OAM Objects.
    OamObject **items = (OamObject**) queue->items; 

    while(index < queue->size)
    {
        if (index == 0) index = 1; // Guard

        uint8_t x_prev = items[index - 1]->x;
        uint8_t x_curr = items[index]->x;

        if (x_prev > x_curr)
        {
            OamObject *obj_prev = items[index - 1]; 
            OamObject *obj_curr = items[index];
            // SWAP
            items[index - 1] = obj_curr;
            items[index] = obj_prev;
            index -= 1;
            continue;
        }

        index += 1;
    }
}

uint8_t queue_size(Queue *queue)
{
    return queue->size;
}

/* Queue Initialization */

Queue *init_queue(uint8_t capacity, QueueOptions queue_type)
{
    Queue *queue    = (Queue*) malloc(sizeof(Queue));

    queue->capacity = capacity;
    queue->front    = -1;
    queue->rear     = -1;
    queue->size     =  0;
    queue->type     = queue_type;

    switch(queue_type)
    {
        case PIXEL:
            queue->items = (void**) malloc(capacity * sizeof(GbcPixel*));
            for (int i = 0; i < capacity; i++)
            {
                queue->items[i] = (void*) malloc(sizeof(GbcPixel));
            }
        break;

        case OBJECT:
            queue->items = (void**) malloc(capacity * sizeof(OamObject*));
            for (int i = 0; i < capacity; i++)
            {
                queue->items[i] = (void*) malloc(sizeof(OamObject));
            }
        break;
    }

    return queue;
}

void tidy_queue(Queue *queue)
{
    for (int i = 0; i < queue->capacity; i++)
    {
        free(queue->items[i]);
        queue->items[i] = NULL;
    }

    free(queue->items);
    queue->items = NULL;
    free(queue);
    queue = NULL;
}


