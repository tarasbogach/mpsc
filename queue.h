#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Ring buffer for variable-sized byte messages
// Message format: [uint32_t length][message bytes]
// Aligned to cache line to reduce false sharing
typedef struct {
    // Producer-only cache line (hot path)
    _Atomic(size_t) write_pos;       // Write position (producer only)
    _Atomic(uint64_t) publish_count; // Total successful publishes
    _Atomic(uint64_t) drop_count;    // Dropped messages count
    _Atomic(uint64_t) publish_bytes; // Total published bytes
    _Atomic(uint64_t) drop_bytes;    // Dropped bytes count
    char _pad1[64 - 40];             // Padding to separate from consumer
    
    // Consumer-only cache line (hot path)
    _Atomic(size_t) read_pos;        // Read position (consumer only)
    uint32_t last_msg_length;        // Cached for queue_ack optimization
    char _pad2[64 - 12];             // Padding to separate from shared
    
    // Shared/cold fields
    uint8_t *buffer;                 // Byte buffer
    size_t capacity;                 // Buffer capacity in bytes (power of 2)
    _Atomic(bool) closed;            // Producer signaled no more messages
    _Atomic(bool) active;            // Queue is currently in use
    int eventfd;                     // Event fd for this queue
    int queue_id;                    // Queue index in pool
} __attribute__((aligned(64))) queue_t;

// Multi-queue system for multiple producers
typedef struct {
    queue_t *queues;                 // Array of ring buffers (one per producer)
    uint8_t *free_bitmap;            // Bitmap of free queue slots (1=free, 0=used, AVX2-aligned)
    size_t bitmap_size_bytes;        // Size of bitmap in bytes (32-byte aligned)
    int capacity;                    // Total capacity (preallocated queues)
    int num_active;                  // Number of active producers
    int num_free;                    // Number of free queues
    size_t queue_capacity;           // Capacity per queue
    int epoll_fd;                    // Epoll fd for consumer
    bool owns_epoll_fd;              // True if we created epoll_fd (should close on destroy)
    pthread_mutex_t lock;            // Protects add/remove operations
    _Atomic(uint64_t) total_published; // Total published messages (lifetime)
    _Atomic(uint64_t) total_dropped;  // Total dropped messages (lifetime)
    _Atomic(uint64_t) total_published_bytes; // Total published bytes (lifetime)
    _Atomic(uint64_t) total_dropped_bytes;   // Total dropped bytes (lifetime)
    _Atomic(uint64_t) total_acquired;  // Total queues ever acquired (lifetime)
} queue_pool_t;

// Initialize a single queue
int queue_init(queue_t *q, size_t capacity, int queue_id);

// Destroy a queue
void queue_destroy(queue_t *q);

// Reset a queue to initial state (clears all messages)
void queue_reset(queue_t *q);

// Enqueue a message (returns false if buffer full - message dropped)
bool queue_publish(queue_t *q, const void *data, size_t size);

// Zero-copy API: Peek at message without copying
// Returns message size and sets ptr/size (with mirrored buffer, always contiguous)
// Returns 0 if queue is empty
// After processing, call queue_ack() to remove message from queue
size_t queue_get(queue_t *q, uint8_t **ptr, size_t *size);

// Commit/dequeue the message after it has been processed
void queue_ack(queue_t *q);

// Get statistics
void queue_stats(queue_t *q, uint64_t *published, uint64_t *dropped, uint64_t *published_bytes, uint64_t *dropped_bytes);


// Signal that producer is done sending messages (called by producer thread)
void queue_close(queue_t *q);

// Check if queue is closed and empty (safe for removal)
bool queue_is_closed_and_empty(queue_t *q);

// Initialize multi-queue system with preallocated pool
// If epoll_fd is -1, creates its own epoll instance (legacy behavior)
// If epoll_fd is valid, uses provided epoll instance (caller manages lifetime)
int queue_pool_init(queue_pool_t *mq, int max_producers, size_t capacity_per_queue, int epoll_fd);

// Destroy multi-queue system
void queue_pool_destroy(queue_pool_t *mq);

// Add a producer (returns queue_id or -1 on error)
int queue_pool_acquire(queue_pool_t *mq);

// Remove a producer (returns 0 on success, -1 if queue not closed/empty or invalid id)
int queue_pool_release(queue_pool_t *mq, int queue_id);

// Get queue by id
queue_t *queue_pool_get(queue_pool_t *mq, int queue_id);

// Get epoll fd for consumer
int queue_pool_get_epoll_fd(queue_pool_t *mq);

// Check if queue is empty
bool queue_pool_is_empty(queue_pool_t *mq, int queue_id);

// Get accumulated statistics across all producers (lifetime)
void queue_pool_stats(queue_pool_t *mq, uint64_t *total_published, uint64_t *total_dropped, 
                      uint64_t *total_published_bytes, uint64_t *total_dropped_bytes,
                      uint64_t *total_acquired);


#endif // QUEUE_H
