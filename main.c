#define _DEFAULT_SOURCE
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define NUM_PRODUCERS 4
#define MESSAGES_PER_PRODUCER 5000
#define RING_BUFFER_SIZE (4 * 1024)  // 4KB per producer - smaller to trigger drops
#define MAX_EVENTS 10
#define MAX_MESSAGE_SIZE 512

// Message structure with sequence number and data for integrity checking
typedef struct {
    uint32_t queue_id;
    uint32_t seq_num;
    uint32_t checksum;
    char data[200];  // Variable length will use different amounts
} test_message_t;

typedef struct {
    queue_pool_t *mq;
    int queue_id;
    int num_messages;
    int delay_us;
} producer_args_t;

// Simple checksum for verification
static uint32_t calculate_checksum(uint32_t queue_id, uint32_t seq_num, const char *data, size_t len) {
    uint32_t sum = queue_id ^ seq_num;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + (uint8_t)data[i];
    }
    return sum;
}



// Producer thread function
void *producer_thread(void *arg) {
    producer_args_t *args = (producer_args_t *)arg;
    queue_t *rb = queue_pool_get(args->mq, args->queue_id);
    
    printf("Producer %d started (sending to dedicated queue)\n", args->queue_id);
    
    int success_count = 0;
    int drop_count = 0;
    
    for (int i = 0; i < args->num_messages; i++) {
        test_message_t msg;
        msg.queue_id = args->queue_id;
        msg.seq_num = i;
        
        // Variable message content size
        size_t data_len = 32 + (i % 169); // Variable data length
        snprintf(msg.data, sizeof(msg.data),
                 "Producer %d, Seq %d, DataLen %zu, Test message content for integrity verification",
                 args->queue_id, i, data_len);
        
        // Calculate checksum
        msg.checksum = calculate_checksum(msg.queue_id, msg.seq_num, msg.data, data_len);
        
        // Variable total message size
        size_t total_size = offsetof(test_message_t, data) + data_len;
        
        if (queue_publish(rb, &msg, total_size)) {
            success_count++;
        } else {
            // Message dropped - buffer full
            drop_count++;
        }
        
        // Simulate work - make producers fast
        if (args->delay_us > 0) {
            if (i % 10 == 0) {  // Only delay occasionally
                usleep(args->delay_us);
            }
        }
    }
    
    printf("Producer %d finished: %d sent, %d dropped locally\n", 
           args->queue_id, success_count, drop_count);
    
    // Signal consumer that no more messages will come
    queue_close(rb);
    
    return NULL;
}

// Consumer function using zero-copy API with epoll
void consumer_loop(queue_pool_t *mq, int expected_messages, int delay_us) {
    int epoll_fd = queue_pool_get_epoll_fd(mq);
    struct epoll_event events[MAX_EVENTS];
    
    printf("Consumer started, monitoring %d queues (ZERO-COPY MODE)\n", mq->capacity);
    
    int received = 0;
    int wrapped_messages = 0;
    int *producer_counts = (int *)calloc(mq->capacity, sizeof(int));
    int *next_seq = (int *)calloc(mq->capacity, sizeof(int));  // Expected next sequence number
    uint64_t total_bytes = 0;
    int checksum_errors = 0;
    int seq_errors = 0;
    
    // Run for a fixed duration or until we think we're done
    int idle_iterations = 0;
    
    while (idle_iterations < 10) {
        // Wait for events (timeout after 100ms)
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
        
        if (nfds == -1) {
            perror("epoll_wait");
            break;
        }
        
        if (nfds == 0) {
            // Timeout - check if we're done
            idle_iterations++;
            continue;
        }
        
        idle_iterations = 0;  // Reset idle counter
        
        // Process events from different queues
        for (int i = 0; i < nfds; i++) {
            if (events[i].events & EPOLLIN) {
                int queue_id = events[i].data.u32;
                queue_t *rb = queue_pool_get(mq, queue_id);
                
                // Drain all available messages from this queue using zero-copy API
                int drained = 0;
                uint8_t *ptr;
                size_t size;
                size_t msg_size;
                
                while ((msg_size = queue_get(rb, &ptr, &size)) > 0) {
                    // Verify message integrity
                    if (msg_size >= offsetof(test_message_t, data)) {
                        test_message_t *msg = (test_message_t *)ptr;
                        
                        // Check sequence number
                        if (msg->seq_num != (uint32_t)next_seq[queue_id]) {
                            seq_errors++;
                            printf("ERROR: Producer %d seq mismatch! Expected %d, got %u\n",
                                   queue_id, next_seq[queue_id], msg->seq_num);
                        }
                        next_seq[queue_id] = msg->seq_num + 1;
                        
                        // Verify checksum
                        size_t data_len = msg_size - offsetof(test_message_t, data);
                        uint32_t expected = calculate_checksum(msg->queue_id, msg->seq_num, 
                                                                msg->data, data_len);
                        if (msg->checksum != expected) {
                            checksum_errors++;
                            printf("ERROR: Producer %d seq %u checksum mismatch! Expected %u, got %u\n",
                                   queue_id, msg->seq_num, expected, msg->checksum);
                        }
                        
                        producer_counts[queue_id]++;
                        received++;
                        drained++;
                        total_bytes += msg_size;
                        
                        if (received % 200 == 0) {
                            printf("Received %d messages (%lu bytes total): [%zu bytes] P%u Seq%u OK\n",
                                   received, total_bytes, msg_size, msg->queue_id, msg->seq_num);
                        }
                    }
                    
                    // Commit the message (advance read pointer)
                    queue_ack(rb);
                }
                
                if (drained > 0 && drained >= 50) {
                    printf("  Drained %d messages from queue %d\n", drained, queue_id);
                }
                
                // Check if producer closed and queue is empty
                if (queue_is_closed_and_empty(rb)) {
                    printf("  Producer %d closed and drained, removing queue\n", queue_id);
                    if (queue_pool_release(mq, queue_id) == 0) {
                        printf("  Producer %d removed by consumer\n", queue_id);
                    }
                }
            }
        }
        
        // Simulate slow consumer
        if (delay_us > 0) {
            usleep(delay_us);
        }
    }
    
    printf("\nConsumer finished. Received %d messages (expected %d), %lu total bytes:\n", 
           received, expected_messages, total_bytes);
    printf("  Integrity: %d checksum errors, %d sequence errors\n", checksum_errors, seq_errors);
    printf("  Wrapped messages (spanning buffer boundary): %d (%.1f%%)\n",
           wrapped_messages, received > 0 ? (100.0 * wrapped_messages / received) : 0.0);
    
    // Print statistics for each producer
    for (int i = 0; i < mq->capacity; i++) {
        queue_t *rb = queue_pool_get(mq, i);
        if (!rb) continue;  // Skip inactive queues
        
        uint64_t published, dropped, published_bytes, dropped_bytes;
        queue_stats(rb, &published, &dropped, &published_bytes, &dropped_bytes);
        uint64_t attempts = published + dropped;
        uint64_t total_bytes_attempted = published_bytes + dropped_bytes;
        
        printf("  Producer %d: received=%d, published=%lu (%lu bytes), dropped=%lu (%lu bytes), drop rate: %.1f%% msgs / %.1f%% bytes\n",
               i, producer_counts[i], published, published_bytes, dropped, dropped_bytes,
               attempts > 0 ? (100.0 * dropped / attempts) : 0.0,
               total_bytes_attempted > 0 ? (100.0 * dropped_bytes / total_bytes_attempted) : 0.0);
    }
    
    printf("\nAverage message size: %.1f bytes\n", 
           received > 0 ? (double)total_bytes / received : 0.0);
    
    free(producer_counts);
    free(next_seq);
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    
    // Parse command line arguments for consumer delay
    int consumer_delay = 0;
    if (argc > 1) {
        consumer_delay = atoi(argv[1]);
    }
    
    // Initialize multi-queue system with preallocated pool
    queue_pool_t mq;
    if (queue_pool_init(&mq, NUM_PRODUCERS * 2, RING_BUFFER_SIZE, -1) != 0) {
        fprintf(stderr, "Failed to initialize multi-queue\n");
        return 1;
    }
    
    // Dynamically add producers
    int queue_ids[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        queue_ids[i] = queue_pool_acquire(&mq);
        if (queue_ids[i] < 0) {
            fprintf(stderr, "Failed to add producer %d\n", i);
            return 1;
        }
    }
    
    printf("Zero-Copy Byte Ring Buffer MPSC Example (MIRRORED VIRTUAL MEMORY)\n");
    printf("==================================================================\n");
    printf("Producers: %d (each with dedicated ring buffer)\n", NUM_PRODUCERS);
    printf("Ring buffer size: %d bytes\n", RING_BUFFER_SIZE);
    printf("Messages per producer: %d\n", MESSAGES_PER_PRODUCER);
    printf("Message size range: 32-231 bytes (variable)\n");
    printf("Consumer delay: %d us\n", consumer_delay);
    printf("Total expected: %d messages\n\n", NUM_PRODUCERS * MESSAGES_PER_PRODUCER);
    
    // Create producer threads with different speeds
    pthread_t producers[NUM_PRODUCERS];
    producer_args_t producer_args[NUM_PRODUCERS];
    
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_args[i].mq = &mq;
        producer_args[i].queue_id = queue_ids[i];  // Use dynamic IDs
        producer_args[i].num_messages = MESSAGES_PER_PRODUCER;
        // Different producers have different speeds
        producer_args[i].delay_us = 50 * (i + 1);
        
        if (pthread_create(&producers[i], NULL, producer_thread, &producer_args[i]) != 0) {
            fprintf(stderr, "Failed to create producer thread %d\n", i);
            return 1;
        }
    }
    
    // Small delay to let producers start
    usleep(10000);
    
    // Run consumer in main thread
    consumer_loop(&mq, NUM_PRODUCERS * MESSAGES_PER_PRODUCER, consumer_delay);
    
    // Wait for all producer threads to finish
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
    }
    
    // Check if any queues remain (consumer should have removed closed ones)
    printf("\nChecking for remaining queues:\n");
    int remaining = 0;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        queue_t *rb = queue_pool_get(&mq, queue_ids[i]);
        if (rb) {
            remaining++;
            printf("  WARNING: Producer %d queue still active\n", queue_ids[i]);
            
            // Drain and remove
            int drained = 0;
            uint8_t *ptr;
            size_t size;
            while (queue_get(rb, &ptr, &size) > 0) {
                drained++;
                queue_ack(rb);
            }
            
            if (drained > 0) {
                printf("    Drained %d remaining messages\n", drained);
            }
            
            queue_pool_release(&mq, queue_ids[i]);
            printf("    Removed\n");
        }
    }
    
    if (remaining == 0) {
        printf("  All producers cleaned up by consumer ✓\n");
    }
    
    // Get lifetime statistics
    uint64_t total_published, total_dropped, total_published_bytes, total_dropped_bytes, total_acquired;
    queue_pool_stats(&mq, &total_published, &total_dropped, &total_published_bytes, &total_dropped_bytes, &total_acquired);
    uint64_t total_attempts = total_published + total_dropped;
    uint64_t total_bytes_attempted = total_published_bytes + total_dropped_bytes;
    printf("\nLifetime statistics (all producers):\n");
    printf("  Total published: %lu messages (%lu bytes)\n", total_published, total_published_bytes);
    printf("  Total dropped: %lu messages (%lu bytes)\n", total_dropped, total_dropped_bytes);
    printf("  Drop rate: %.1f%% messages / %.1f%% bytes\n", 
           total_attempts > 0 ? (100.0 * total_dropped / total_attempts) : 0.0,
           total_bytes_attempted > 0 ? (100.0 * total_dropped_bytes / total_bytes_attempted) : 0.0);
    printf("  Total queues acquired: %lu\n", total_acquired);
    
    // Cleanup
    queue_pool_destroy(&mq);
    
    printf("\nAll done!\n");
    return 0;
}
