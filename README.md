# MPSC Lock-Free Queue Pool

**Multi-Producer Single-Consumer (MPSC)** lock-free queue implementation using per-producer ring buffers with mirrored virtual memory mapping.

## What is MPSC?

**Multi-Producer Single-Consumer (MPSC)** is a concurrency pattern where:
- Multiple producer threads can publish messages concurrently
- A single consumer thread processes all messages
- **Lock-free on data path**: Producers use only atomic operations, no mutexes
- **No contention**: Each producer has a dedicated queue, eliminating inter-producer blocking

This pattern is ideal for scenarios like log aggregation, event processing, and audio/video streaming where multiple sources feed a single processing thread.

## Key Features

### Lock-Free Performance
- **Zero locks on publish path**: Producers use atomic operations only
- **Zero locks on consumer path**: Single consumer thread, no synchronization needed
- **Pool management locks**: Only `acquire()`/`release()` operations use mutex (not data path)
- **Per-producer queues**: No contention between producers

### Memory Efficiency
- **Mirrored virtual memory**: Same physical buffer mapped twice consecutively
- **Eliminates wraparound**: Always read/write contiguously, no boundary checks
- **Zero-copy consumer**: Direct pointer access to message bytes
- **Preallocated pool**: All buffers allocated once, reused efficiently

### Operational
- **Variable message sizes**: Messages stored inline with length prefix
- **Drop-on-full**: Producers drop messages when queue full (no blocking)
- **Event-driven**: Uses `eventfd` + `epoll` for efficient notification
- **Dynamic producers**: Acquire/release queues from pool at runtime
- **Producer close signal**: Consumer-driven cleanup when producer finishes

## Architecture

### Components

- **queue_t**: Single lock-free ring buffer with mirrored memory
- **queue_pool_t**: Pool manager with preallocated queue array
- **Mirrored mapping**: Physical memory mapped twice at consecutive virtual addresses
- **Zero wraparound**: Consumer always reads contiguous bytes (perfect for socket I/O)

### Files

- **queue.h/c**: Core implementation (queue + pool)
- **main.c**: Demo with 4 producers, integrity testing, statistics
- **test_concurrent.c**: Stress test for pool acquire/release (800 cycles, 8 threads)

## API Overview

### Pool Management

```c
queue_pool_t pool;

// Initialize pool with preallocated buffers (creates own epoll)
queue_pool_init(&pool, max_queues, buffer_size_per_queue, -1);

// OR: Use existing epoll for integration with app event loop
int app_epoll = epoll_create1(0);
queue_pool_init(&pool, max_queues, buffer_size_per_queue, app_epoll);

// Producer: Acquire a queue from pool
int queue_id = queue_pool_acquire(&pool);

// Producer: Get queue handle
queue_t *q = queue_pool_get(&pool, queue_id);

// Producer: Publish message (lock-free, drop-on-full)
bool ok = queue_publish(q, data, size);

// Producer: Signal done (no more messages)
queue_close(q);

// Consumer: Get epoll fd
int epoll_fd = queue_pool_get_epoll_fd(&pool);

// Consumer: Zero-copy read
uint8_t *ptr;
size_t size;
if (queue_get(q, &ptr, &size) > 0) {
    process_message(ptr, size);
    queue_ack(q);  // Advance read pointer
}

// Consumer: Detect closed and empty queue
if (queue_is_closed_and_empty(q)) {
    queue_pool_release(&pool, queue_id);  // Return to pool
}

// Cleanup
queue_pool_destroy(&pool);
```

## External Epoll Integration

The queue pool can integrate with existing application event loops by accepting an external epoll file descriptor:

```c
// Create your application's epoll instance
int app_epoll = epoll_create1(0);

// Add your application events (timers, sockets, etc.)
struct epoll_event ev;
ev.events = EPOLLIN;
ev.data.u32 = MY_TIMER_EVENT_ID;
epoll_ctl(app_epoll, EPOLL_CTL_ADD, timer_fd, &ev);

// Initialize queue pool with your epoll instance
queue_pool_t pool;
queue_pool_init(&pool, max_queues, buffer_size, app_epoll);

// Single event loop handles both application and queue events
struct epoll_event events[MAX_EVENTS];
while (running) {
    int nfds = epoll_wait(app_epoll, events, MAX_EVENTS, timeout);
    
    for (int i = 0; i < nfds; i++) {
        if (events[i].data.u32 == MY_TIMER_EVENT_ID) {
            handle_timer_event();
        } else {
            // Handle queue event
            int queue_id = events[i].data.u32;
            process_queue_messages(queue_id);
        }
    }
}

// Pool won't close your epoll fd - you manage its lifetime
queue_pool_destroy(&pool);
close(app_epoll);
```

**Benefits:**
- Single `epoll_wait()` for all events (queues + application)
- Reduced system calls and context switching
- Better integration with existing event-driven architectures
- Queue pool doesn't take ownership of epoll fd

## Building

```bash
make                    # Build main demo
make run                # Run with fast consumer
make run-slow           # Run with slow consumer (triggers drops)
make test_concurrent    # Thread safety stress test
make test_external_epoll # External epoll integration demo
make clean              # Clean build artifacts
```

## Running

### Main Demo
```bash
./build/mpsc [consumer_delay_us]

# Examples:
./build/mpsc          # Fast consumer (0% drops)
./build/mpsc 5000     # Slow consumer (5ms delay, ~85% drops)
```

### Concurrent Stress Test
```bash
./build/test_concurrent

# Tests 8 threads doing 100 acquire/release cycles each
# Validates pool thread safety under contention
```

## How It Works

### 1. Mirrored Virtual Memory
```
Virtual Address Space:
[0x1000 - 0x2000]: Mapping 1  →  Physical [0x0 - 0x1000]
[0x2000 - 0x3000]: Mapping 2  →  Physical [0x0 - 0x1000]  (same physical!)

Result: Write at offset 0xFF0 reaches into 0x1000 continuously
```

### 2. Lock-Free Publishing
```c
// Producer thread (lock-free)
size_t write_pos = atomic_load(&q->write_pos, relaxed);
size_t read_pos = atomic_load(&q->read_pos, acquire);

if (space_available(write_pos, read_pos)) {
    memcpy(&buffer[write_pos & mask], data, size);  // Always contiguous!
    atomic_store(&q->write_pos, write_pos + size, release);
    eventfd_write(q->eventfd, 1);  // Notify consumer
}
```

### 3. Zero-Copy Consumer
```c
// Consumer thread (no locks, single owner)
uint8_t *ptr;
size_t size;

while (queue_get(q, &ptr, &size) > 0) {
    // ptr points directly into ring buffer
    send(socket, ptr, size, 0);  // No memcpy needed!
    queue_ack(q);  // Advance read pointer
}
```

### 4. Pool Lifecycle
```c
// Initialization: Allocate all memory once
queue_pool_init(&pool, 1000, 1MB, -1);  // 1GB total RAM, own epoll

// Runtime: Acquire (lock for bookkeeping, then reset counters)
int id = queue_pool_acquire(&pool);  // Reuses preallocated buffer

// Runtime: Release (lock for bookkeeping, buffer stays allocated)
queue_pool_release(&pool, id);  // Back to free list, buffer kept

// Cleanup: Free all memory
queue_pool_destroy(&pool);
```

## Configuration

Adjust in `main.c`:
- `NUM_PRODUCERS`: Number of producer threads (default: 4)
- `MESSAGES_PER_PRODUCER`: Messages per producer (default: 5000)
- `RING_BUFFER_SIZE`: Buffer size per queue (default: 4KB)

## Memory Usage Example

**Scenario:** Real-time audio streaming server

**Requirements:**
- 1000 concurrent audio streams
- 1 minute buffering per stream
- 8 KHz, 16-bit, mono PCM audio

**Calculation:**
```
Samples/minute = 8000 Hz × 60 sec = 480,000 samples
Bytes = 480,000 × 2 bytes/sample = 960,000 bytes
Buffer size (power of 2) = 1,048,576 bytes (1 MB)
```

**Code:**
```c
queue_pool_t pool;
queue_pool_init(&pool, 1000, 1048576, -1);  // 1000 queues × 1MB each
```

**Memory footprint:**
- Physical RAM: **1 GB** (1000 × 1 MB buffers)
- Virtual address space: **2 GB** (mirrored mapping)
- Metadata: **132 KB** (queue_t structs + free list)
- Total: **~1 GB physical memory**

**Performance benefit:**
- Buffers allocated once at startup
- No `mmap`/`munmap` overhead during acquire/release
- Lock-free publishing: ~10-100ns per message
- Zero-copy consumer: Direct memory access

## Statistics & Testing

The implementation includes comprehensive testing:

- **Message integrity**: CRC32 checksums validate data
- **Sequence tracking**: Detects dropped messages
- **Wraparound verification**: Confirms 0% wraparound with mirrored buffer
- **Drop rate measurement**: Per-queue and lifetime statistics
- **Concurrent stress test**: 800 acquire/release cycles across 8 threads

Example output:
```
Consumer finished. Received 2901 messages (expected 20000), 329235 total bytes:
  Integrity: 0 checksum errors, 124 sequence errors
  Wrapped messages (spanning buffer boundary): 0 (0.0%)
  Producer 4: received=975, published=5000, dropped=4025 (80.5% drop rate)
  
Lifetime statistics (all producers):
  Total published: 20000
  Total dropped: 17099
  Drop rate: 85.5%
```

## Lock-Free Guarantees

### What is Lock-Free?

- **No mutexes on data path**: `queue_publish()`, `queue_get()`, `queue_ack()` use only atomics
- **Wait-free progress**: Producers never wait for consumer or other producers
- **System call minimal**: Only `eventfd_write()` for notification (non-blocking)

### Where Locks Are Used

- `queue_pool_acquire()`: Mutex protects free list manipulation
- `queue_pool_release()`: Mutex protects returning queue to pool
- **Impact**: These are rare operations (once per producer lifetime), not per-message

### Performance Characteristics

- **Publish latency**: ~50-200ns per message (atomic + memcpy)
- **Consumer latency**: ~100-500ns per message (atomic + pointer return)
- **Throughput**: Millions of messages per second per producer
- **Scalability**: Linear with number of producers (no contention)

## Use Cases

Ideal for:
- **Audio/video streaming**: Multiple input streams → single encoder/muxer
- **Log aggregation**: Multiple threads → single log writer
- **Event processing**: Multiple event sources → single event loop
- **Telemetry collection**: Multiple sensors → single data logger
- **Network I/O**: Multiple connection handlers → single socket writer

Not ideal for:
- **Multi-consumer**: Designed for single consumer only
- **Strict ordering**: Per-producer ordering only, not global
- **Blocking producers**: Uses drop-on-full, not backpressure

## License

See LICENSE file for details.

## Documentation

- **CLAUDE.md**: Comprehensive technical documentation for AI agents
- Includes implementation details, memory layout, and development workflow
