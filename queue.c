#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "queue.h"
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <immintrin.h>  // AVX2 intrinsics

// Helper: calculate available space
// Unsigned arithmetic naturally handles position wraparound
static inline size_t get_available_space(size_t read_pos, size_t write_pos, size_t capacity) {
    return capacity - (write_pos - read_pos) - 1;
}

// Helper: calculate used space
// Unsigned arithmetic naturally handles position wraparound
static inline size_t get_used_space(size_t read_pos, size_t write_pos, size_t capacity) {
    (void)capacity;  // Unused with optimized calculation
    return write_pos - read_pos;
}

// Bitmap helpers for free list management (AVX2-optimized)
// Bitmap convention: 1 = free, 0 = used

// Round up to next multiple of 32 (for AVX2 alignment)
static inline size_t bitmap_size_bytes(int capacity) {
    size_t bits = (size_t)capacity;
    size_t bytes = (bits + 7) / 8;  // Round up to bytes
    return (bytes + 31) & ~31;      // Round up to 32-byte boundary
}

// Set bit at index (mark as free)
static inline void bitmap_set(uint8_t *bitmap, int index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

// Clear bit at index (mark as used)
static inline void bitmap_clear(uint8_t *bitmap, int index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

// Test bit at index
static inline bool bitmap_test(uint8_t *bitmap, int index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

// Count total set bits using AVX2 (always use SIMD, no branching)
static inline int bitmap_count_bits(uint8_t *bitmap, size_t size_bytes, int max_index) {
    int count = 0;

    // Process 32 bytes (256 bits) at a time with AVX2
    size_t avx_chunks = size_bytes / 32;
    for (size_t i = 0; i < avx_chunks; i++) {
        __m256i chunk = _mm256_load_si256((__m256i *)(bitmap + i * 32));

        // Use 64-bit popcount on each 64-bit section
        uint64_t *ptr = (uint64_t *)&chunk;
        count += _mm_popcnt_u64(ptr[0]);
        count += _mm_popcnt_u64(ptr[1]);
        count += _mm_popcnt_u64(ptr[2]);
        count += _mm_popcnt_u64(ptr[3]);
    }

    // Mask off bits beyond max_index
    int valid_bits = max_index;
    int total_bits = size_bytes * 8;
    if (valid_bits < total_bits) {
        // Count excess bits we need to subtract
        int excess_start = valid_bits;
        int excess_count = 0;
        for (int i = excess_start; i < total_bits; i++) {
            if (bitmap_test(bitmap, i)) {
                excess_count++;
            }
        }
        count -= excess_count;
    }

    return count;
}

// Find first set bit (first free queue) using AVX2
// Returns -1 if no bit is set
static inline int bitmap_find_first_set(uint8_t *bitmap, size_t size_bytes, int max_index) {
    // Process 32 bytes (256 bits) at a time
    size_t avx_chunks = size_bytes / 32;
    for (size_t i = 0; i < avx_chunks; i++) {
        __m256i chunk = _mm256_load_si256((__m256i *)(bitmap + i * 32));

        // Test if any byte is non-zero
        uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, _mm256_setzero_si256()));

        if (mask != 0xFFFFFFFF) {  // At least one byte is non-zero
            // Find first non-zero byte
            uint32_t inv_mask = ~mask;
            int byte_offset = __builtin_ctz(inv_mask);
            int result = (i * 32 + byte_offset) * 8 + __builtin_ctz(bitmap[i * 32 + byte_offset]);

            return (result < max_index) ? result : -1;
        }
    }

    return -1;  // No free slot found
}

int queue_init(queue_t *q, size_t capacity, int queue_id) {
    // Ensure capacity is power of 2 for efficient masking
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        return -1; // Must be power of 2
    }

    // Create mirrored virtual ring buffer using mmap
    // Reserve 2x capacity in virtual address space
    void *addr = mmap(NULL, 2 * capacity, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return -1;
    }

    // Create memory file descriptor (faster than shm_open, auto-cleanup)
    int fd = memfd_create("mpsc_ring", MFD_CLOEXEC);
    if (fd == -1) {
        munmap(addr, 2 * capacity);
        return -1;
    }

    // Set the size of shared memory
    if (ftruncate(fd, capacity) == -1) {
        close(fd);
        munmap(addr, 2 * capacity);
        return -1;
    }

    // Map first half
    void *addr1 = mmap(addr, capacity, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, 0);
    if (addr1 == MAP_FAILED || addr1 != addr) {
        close(fd);
        munmap(addr, 2 * capacity);
        return -1;
    }

    // Map second half (mirror of first half)
    void *addr2 = mmap((uint8_t *)addr + capacity, capacity, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, 0);
    if (addr2 == MAP_FAILED || addr2 != (void *)((uint8_t *)addr + capacity)) {
        munmap(addr, 2 * capacity);
        close(fd);
        return -1;
    }

    close(fd); // Can close fd now, mappings remain

    q->buffer = (uint8_t *)addr;

    q->capacity = capacity;
    atomic_init(&q->write_pos, 0);
    atomic_init(&q->read_pos, 0);
    atomic_init(&q->publish_count, 0);
    atomic_init(&q->drop_count, 0);
    atomic_init(&q->publish_bytes, 0);
    atomic_init(&q->drop_bytes, 0);
    atomic_init(&q->closed, false);
    atomic_init(&q->active, false);  // Not active until acquired
    q->queue_id = queue_id;

    // Create eventfd for this queue (non-blocking, semaphore mode)
    q->eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (q->eventfd == -1) {
        munmap(q->buffer, 2 * capacity);
        return -1;
    }

    return 0;
}

void queue_reset(queue_t *q) {
    // Reset atomic counters to initial state (reuse buffer memory)
    atomic_store_explicit(&q->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->read_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->publish_count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->drop_count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->publish_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&q->drop_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&q->closed, false, memory_order_relaxed);
    atomic_store_explicit(&q->active, true, memory_order_release);

    // Buffer and eventfd remain allocated for reuse
}


void queue_destroy(queue_t *q) {
    if (q->buffer) {
        munmap(q->buffer, 2 * q->capacity);
        q->buffer = NULL;
    }

    if (q->eventfd != -1) {
        close(q->eventfd);
        q->eventfd = -1;
    }
}

__attribute__((hot))
bool queue_publish(queue_t *q, const void *data, size_t size) {
    if (!data || size == 0 || size > (q->capacity / 2)) {
        return false; // Message too large
    }

    // Load current positions
    size_t write_pos = atomic_load_explicit(&q->write_pos, memory_order_relaxed);
    size_t read_pos = atomic_load_explicit(&q->read_pos, memory_order_acquire);

    // Calculate space needed: 4 bytes for length + message size
    size_t total_size = sizeof(uint32_t) + size;
    size_t available = get_available_space(read_pos, write_pos, q->capacity);

    // Check if we have enough space
    if (available < total_size) {
        // Buffer full - drop message
        atomic_fetch_add_explicit(&q->drop_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&q->drop_bytes, size, memory_order_relaxed);
        return false;
    }

    // Increment publish count and bytes only on success path
    atomic_fetch_add_explicit(&q->publish_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&q->publish_bytes, size, memory_order_relaxed);

    // With mirrored buffer, write contiguously without wraparound
    size_t pos = write_pos & (q->capacity - 1);

    // Write length header
    uint32_t msg_length = (uint32_t)size;
    *(uint32_t *)(&q->buffer[pos]) = msg_length;

    // Write message data
    memcpy(&q->buffer[pos + sizeof(uint32_t)], data, size);

    // Update write position
    size_t new_write_pos = write_pos + sizeof(uint32_t) + size;
    atomic_store_explicit(&q->write_pos, new_write_pos, memory_order_release);

    // Signal eventfd
    uint64_t value = 1;
    ssize_t ret = write(q->eventfd, &value, sizeof(value));
    (void)ret; // Best effort notification

    return true;
}

__attribute__((hot))
size_t queue_get(queue_t *q, uint8_t **ptr, size_t *size) {
    // Initialize outputs
    *ptr = NULL;
    *size = 0;

    // Load current positions
    size_t read_pos = atomic_load_explicit(&q->read_pos, memory_order_relaxed);
    size_t write_pos = atomic_load_explicit(&q->write_pos, memory_order_acquire);

    size_t used = get_used_space(read_pos, write_pos, q->capacity);

    // Check if we have at least a length header
    if (used < sizeof(uint32_t)) {
        return 0;
    }

    // With mirrored buffer, we can read contiguously without wrapping
    size_t pos = read_pos & (q->capacity - 1);
    
    // Prefetch message data
    __builtin_prefetch(&q->buffer[pos], 0, 3);

    // Read length header directly
    uint32_t msg_length = *(uint32_t *)(&q->buffer[pos]);

    // Check if we have the full message
    if (used < sizeof(uint32_t) + msg_length) {
        return 0;
    }

    // Cache length for queue_ack optimization
    q->last_msg_length = msg_length;

    // Message data starts after length header - always contiguous!
    *ptr = &q->buffer[pos + sizeof(uint32_t)];
    *size = msg_length;

    return msg_length;
}

__attribute__((hot))
void queue_ack(queue_t *q) {
    // Load current position
    size_t read_pos = atomic_load_explicit(&q->read_pos, memory_order_relaxed);

    // Use cached length from queue_get (no re-read needed)
    size_t new_read_pos = read_pos + sizeof(uint32_t) + q->last_msg_length;
    atomic_store_explicit(&q->read_pos, new_read_pos, memory_order_release);

    // Consume one event from eventfd
    uint64_t value;
    ssize_t ret = read(q->eventfd, &value, sizeof(value));
    (void)ret;
}


void queue_stats(queue_t *q, uint64_t *published, uint64_t *dropped, uint64_t *published_bytes, uint64_t *dropped_bytes) {
    if (published) {
        *published = atomic_load_explicit(&q->publish_count, memory_order_relaxed);
    }
    if (dropped) {
        *dropped = atomic_load_explicit(&q->drop_count, memory_order_relaxed);
    }
    if (published_bytes) {
        *published_bytes = atomic_load_explicit(&q->publish_bytes, memory_order_relaxed);
    }
    if (dropped_bytes) {
        *dropped_bytes = atomic_load_explicit(&q->drop_bytes, memory_order_relaxed);
    }
}

int queue_pool_init(queue_pool_t *mq, int max_producers, size_t capacity_per_queue, int epoll_fd) {
    if (max_producers <= 0) {
        return -1;
    }

    mq->capacity = max_producers;
    mq->num_active = 0;
    mq->num_free = max_producers;
    mq->queue_capacity = capacity_per_queue;
    atomic_init(&mq->total_published, 0);
    atomic_init(&mq->total_dropped, 0);
    atomic_init(&mq->total_published_bytes, 0);
    atomic_init(&mq->total_dropped_bytes, 0);
    atomic_init(&mq->total_acquired, 0);

    // Handle epoll_fd - use provided or create our own
    if (epoll_fd == -1) {
        mq->epoll_fd = epoll_create1(0);
        if (mq->epoll_fd == -1) {
            return -1;
        }
        mq->owns_epoll_fd = true;
    } else {
        mq->epoll_fd = epoll_fd;
        mq->owns_epoll_fd = false;
    }

    // Initialize mutex
    if (pthread_mutex_init(&mq->lock, NULL) != 0) {
        if (mq->owns_epoll_fd) {
            close(mq->epoll_fd);
        }
        return -1;
    }

    // Preallocate queues
    mq->queues = (queue_t *)calloc(max_producers, sizeof(queue_t));
    if (!mq->queues) {
        if (mq->owns_epoll_fd) {
            close(mq->epoll_fd);
        }
        pthread_mutex_destroy(&mq->lock);
        return -1;
    }

    // Preallocate all mirrored buffers upfront (for reuse)
    for (int i = 0; i < max_producers; i++) {
        if (queue_init(&mq->queues[i], capacity_per_queue, i) != 0) {
            // Cleanup already initialized queues on failure
            for (int j = 0; j < i; j++) {
                queue_destroy(&mq->queues[j]);
            }
            free(mq->queues);
            if (mq->owns_epoll_fd) {
                close(mq->epoll_fd);
            }
            pthread_mutex_destroy(&mq->lock);
            return -1;
        }
    }

    // Allocate bitmap for free list tracking (32-byte aligned for AVX2)
    mq->bitmap_size_bytes = bitmap_size_bytes(max_producers);
    mq->free_bitmap = (uint8_t *)aligned_alloc(32, mq->bitmap_size_bytes);
    if (!mq->free_bitmap) {
        for (int i = 0; i < max_producers; i++) {
            queue_destroy(&mq->queues[i]);
        }
        free(mq->queues);
        if (mq->owns_epoll_fd) {
            close(mq->epoll_fd);
        }
        pthread_mutex_destroy(&mq->lock);
        return -1;
    }

    // Initialize bitmap: all bits set to 1 (all queues free)
    memset(mq->free_bitmap, 0xFF, mq->bitmap_size_bytes);

    return 0;
}

void queue_pool_destroy(queue_pool_t *mq) {
    if (mq->queues) {
        // Accumulate stats from all queues and destroy them
        for (int i = 0; i < mq->capacity; i++) {
            uint64_t published, dropped, published_bytes, dropped_bytes;
            queue_stats(&mq->queues[i], &published, &dropped, &published_bytes, &dropped_bytes);
            atomic_fetch_add_explicit(&mq->total_published, published, memory_order_relaxed);
            atomic_fetch_add_explicit(&mq->total_dropped, dropped, memory_order_relaxed);
            atomic_fetch_add_explicit(&mq->total_published_bytes, published_bytes, memory_order_relaxed);
            atomic_fetch_add_explicit(&mq->total_dropped_bytes, dropped_bytes, memory_order_relaxed);
            queue_destroy(&mq->queues[i]);
        }
        free(mq->queues);
        mq->queues = NULL;
    }

    if (mq->free_bitmap) {
        free(mq->free_bitmap);
        mq->free_bitmap = NULL;
    }

    if (mq->epoll_fd != -1 && mq->owns_epoll_fd) {
        close(mq->epoll_fd);
        mq->epoll_fd = -1;
    }

    pthread_mutex_destroy(&mq->lock);
}

int queue_pool_acquire(queue_pool_t *mq) {
    pthread_mutex_lock(&mq->lock);

    if (mq->num_free == 0) {
        pthread_mutex_unlock(&mq->lock);
        return -1; // No free queues
    }

    // Find first free queue using AVX2 bitmap scan
    int queue_id = bitmap_find_first_set(mq->free_bitmap, mq->bitmap_size_bytes, mq->capacity);
    if (queue_id == -1) {
        pthread_mutex_unlock(&mq->lock);
        return -1; // No free queues (shouldn't happen if num_free > 0)
    }

    // Mark queue as used (clear bit)
    bitmap_clear(mq->free_bitmap, queue_id);
    mq->num_free--;
    mq->num_active++;

    pthread_mutex_unlock(&mq->lock);

    // Increment acquisition counter (lifetime statistic)
    atomic_fetch_add_explicit(&mq->total_acquired, 1, memory_order_relaxed);
    
    // Reset the queue state (buffer already allocated, reuse it)
    queue_reset(&mq->queues[queue_id]);

    // Add to epoll (epoll_ctl is thread-safe)
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = queue_id;

    if (epoll_ctl(mq->epoll_fd, EPOLL_CTL_ADD, mq->queues[queue_id].eventfd, &ev) == -1) {
        queue_destroy(&mq->queues[queue_id]);
        pthread_mutex_lock(&mq->lock);
        bitmap_set(mq->free_bitmap, queue_id);
        mq->num_free++;
        mq->num_active--;
        pthread_mutex_unlock(&mq->lock);
        return -1;
    }

    return queue_id;
}

int queue_pool_release(queue_pool_t *mq, int queue_id) {
    if (queue_id < 0 || queue_id >= mq->capacity) {
        return -1;
    }

    pthread_mutex_lock(&mq->lock);

    if (!atomic_load_explicit(&mq->queues[queue_id].active, memory_order_acquire)) {
        pthread_mutex_unlock(&mq->lock);
        return -1; // Queue not active
    }

    // Check if queue is closed and empty
    if (!queue_is_closed_and_empty(&mq->queues[queue_id])) {
        pthread_mutex_unlock(&mq->lock);
        return -1; // Queue must be closed and empty before removal
    }

    // Accumulate statistics before releasing queue
    uint64_t published, dropped, published_bytes, dropped_bytes;
    queue_stats(&mq->queues[queue_id], &published, &dropped, &published_bytes, &dropped_bytes);
    atomic_fetch_add_explicit(&mq->total_published, published, memory_order_relaxed);
    atomic_fetch_add_explicit(&mq->total_dropped, dropped, memory_order_relaxed);
    atomic_fetch_add_explicit(&mq->total_published_bytes, published_bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&mq->total_dropped_bytes, dropped_bytes, memory_order_relaxed);

    // Remove from epoll (epoll_ctl is thread-safe)
    epoll_ctl(mq->epoll_fd, EPOLL_CTL_DEL, mq->queues[queue_id].eventfd, NULL);

    // Mark as inactive (buffer memory remains allocated for reuse)
    atomic_store_explicit(&mq->queues[queue_id].active, false, memory_order_release);

    // Mark queue as free (set bit)
    bitmap_set(mq->free_bitmap, queue_id);
    mq->num_free++;
    mq->num_active--;

    pthread_mutex_unlock(&mq->lock);

    return 0;
}

queue_t *queue_pool_get(queue_pool_t *mq, int queue_id) {
    if (queue_id < 0 || queue_id >= mq->capacity) {
        return NULL;
    }
    if (!atomic_load_explicit(&mq->queues[queue_id].active, memory_order_acquire)) {
        return NULL; // Queue not active
    }
    return &mq->queues[queue_id];
}

int queue_pool_get_epoll_fd(queue_pool_t *mq) {
    return mq->epoll_fd;
}

bool queue_pool_is_empty(queue_pool_t *mq, int queue_id) {
    if (queue_id < 0 || queue_id >= mq->capacity) {
        return true;
    }

    queue_t *q = &mq->queues[queue_id];
    if (!atomic_load_explicit(&q->active, memory_order_acquire)) {
        return true;
    }

    size_t read_pos = atomic_load_explicit(&q->read_pos, memory_order_relaxed);
    size_t write_pos = atomic_load_explicit(&q->write_pos, memory_order_acquire);

    return read_pos == write_pos;
}

void queue_pool_stats(queue_pool_t *mq, uint64_t *total_published, uint64_t *total_dropped,
                      uint64_t *total_published_bytes, uint64_t *total_dropped_bytes,
                      uint64_t *total_acquired) {
    // Get accumulated stats from released producers
    uint64_t published = atomic_load_explicit(&mq->total_published, memory_order_relaxed);
    uint64_t dropped = atomic_load_explicit(&mq->total_dropped, memory_order_relaxed);
    uint64_t published_bytes = atomic_load_explicit(&mq->total_published_bytes, memory_order_relaxed);
    uint64_t dropped_bytes = atomic_load_explicit(&mq->total_dropped_bytes, memory_order_relaxed);

    // Add current stats from active producers
    for (int i = 0; i < mq->capacity; i++) {
        queue_t *q = &mq->queues[i];
        if (atomic_load_explicit(&q->active, memory_order_acquire)) {
            // Direct atomic loads instead of function call
            published += atomic_load_explicit(&q->publish_count, memory_order_relaxed);
            dropped += atomic_load_explicit(&q->drop_count, memory_order_relaxed);
            published_bytes += atomic_load_explicit(&q->publish_bytes, memory_order_relaxed);
            dropped_bytes += atomic_load_explicit(&q->drop_bytes, memory_order_relaxed);
        }
    }

    if (total_published) {
        *total_published = published;
    }
    if (total_dropped) {
        *total_dropped = dropped;
    }
    if (total_published_bytes) {
        *total_published_bytes = published_bytes;
    }
    if (total_dropped_bytes) {
        *total_dropped_bytes = dropped_bytes;
    }
    if (total_acquired) {
        *total_acquired = atomic_load_explicit(&mq->total_acquired, memory_order_relaxed);
    }
}

void queue_close(queue_t *q) {
    // Signal that no more messages will be sent
    atomic_store_explicit(&q->closed, true, memory_order_release);

    // Send notification to wake up consumer
    uint64_t value = 1;
    ssize_t ret = write(q->eventfd, &value, sizeof(value));
    (void)ret;
}

bool queue_is_closed_and_empty(queue_t *q) {
    // Check if producer signaled close
    bool is_closed = atomic_load_explicit(&q->closed, memory_order_acquire);
    if (!is_closed) {
        return false;
    }

    // Check if queue is empty
    size_t read_pos = atomic_load_explicit(&q->read_pos, memory_order_relaxed);
    size_t write_pos = atomic_load_explicit(&q->write_pos, memory_order_acquire);

    return read_pos == write_pos;
}
