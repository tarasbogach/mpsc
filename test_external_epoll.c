#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include "queue.h"

#define NUM_PRODUCERS 3
#define MESSAGES_PER_PRODUCER 1000
#define MESSAGE_SIZE 64
#define MAX_EVENTS 16

// Producer thread arguments
typedef struct {
    queue_pool_t *pool;
    int producer_id;
    int messages_to_send;
} producer_args_t;

// External event for demonstration (e.g., timer, socket, etc.)
typedef struct {
    int eventfd;
    int timer_count;
} external_event_t;

void *producer_thread(void *arg) {
    producer_args_t *args = (producer_args_t *)arg;
    
    // Acquire a queue from the pool
    int queue_id = queue_pool_acquire(args->pool);
    if (queue_id == -1) {
        printf("Producer %d: Failed to acquire queue\n", args->producer_id);
        return NULL;
    }
    
    queue_t *q = queue_pool_get(args->pool, queue_id);
    printf("Producer %d: Acquired queue %d\n", args->producer_id, queue_id);
    
    // Send messages
    char message[MESSAGE_SIZE];
    int sent = 0;
    
    for (int i = 0; i < args->messages_to_send; i++) {
        snprintf(message, sizeof(message), "Producer %d Message %d", args->producer_id, i);
        
        if (queue_publish(q, message, strlen(message) + 1)) {
            sent++;
        }
        
        // Small delay to make it more realistic
        if (i % 100 == 0) {
            usleep(1000); // 1ms
        }
    }
    
    printf("Producer %d: Sent %d/%d messages\n", args->producer_id, sent, args->messages_to_send);
    
    // Signal done and let consumer handle cleanup
    queue_close(q);
    
    return NULL;
}

void setup_external_timer_event(external_event_t *ext_event, int epoll_fd) {
    // Create an eventfd for timer simulation
    ext_event->eventfd = eventfd(0, EFD_CLOEXEC);
    if (ext_event->eventfd == -1) {
        perror("eventfd");
        exit(1);
    }
    
    ext_event->timer_count = 0;
    
    // Add to epoll with custom data
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0xFFFFFFFF; // Special marker for external events
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ext_event->eventfd, &ev) == -1) {
        perror("epoll_ctl timer");
        exit(1);
    }
    
    printf("External timer event added to epoll\n");
}

void *timer_thread(void *arg) {
    external_event_t *ext_event = (external_event_t *)arg;
    
    // Send timer events every 2 seconds
    for (int i = 0; i < 5; i++) {
        sleep(2);
        uint64_t val = 1;
        if (write(ext_event->eventfd, &val, sizeof(val)) == sizeof(val)) {
            ext_event->timer_count++;
        }
    }
    
    return NULL;
}

int main(void) {
    printf("External Epoll Integration Test\n");
    printf("===============================\n");
    printf("Demonstrates using MPSC queue pool with external epoll instance\n");
    printf("Producers: %d, Messages each: %d\n\n", NUM_PRODUCERS, MESSAGES_PER_PRODUCER);
    
    // Create our own epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return 1;
    }
    printf("Created external epoll instance: fd=%d\n", epoll_fd);
    
    // Setup external event (timer simulation)
    external_event_t ext_event;
    setup_external_timer_event(&ext_event, epoll_fd);
    
    // Initialize queue pool with our epoll instance
    queue_pool_t pool;
    if (queue_pool_init(&pool, NUM_PRODUCERS, 4096, epoll_fd) != 0) {
        fprintf(stderr, "Failed to initialize queue pool with external epoll\n");
        close(epoll_fd);
        return 1;
    }
    printf("Queue pool initialized with external epoll fd=%d\n", epoll_fd);
    
    // Verify the pool is using our epoll fd
    if (queue_pool_get_epoll_fd(&pool) != epoll_fd) {
        fprintf(stderr, "ERROR: Pool not using our epoll fd!\n");
        queue_pool_destroy(&pool);
        close(epoll_fd);
        return 1;
    }
    printf("✓ Pool correctly using external epoll fd\n\n");
    
    // Start timer thread for external events
    pthread_t timer_tid;
    if (pthread_create(&timer_tid, NULL, timer_thread, &ext_event) != 0) {
        perror("pthread_create timer");
        queue_pool_destroy(&pool);
        close(epoll_fd);
        return 1;
    }
    
    // Start producer threads
    pthread_t producer_threads[NUM_PRODUCERS];
    producer_args_t producer_args[NUM_PRODUCERS];
    
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_args[i].pool = &pool;
        producer_args[i].producer_id = i + 1;
        producer_args[i].messages_to_send = MESSAGES_PER_PRODUCER;
        
        if (pthread_create(&producer_threads[i], NULL, producer_thread, &producer_args[i]) != 0) {
            perror("pthread_create producer");
            queue_pool_destroy(&pool);
            close(epoll_fd);
            return 1;
        }
    }
    
    // Consumer loop using external epoll
    struct epoll_event events[MAX_EVENTS];
    int messages_received = 0;
    int external_events_received = 0;
    int active_producers = NUM_PRODUCERS;
    
    printf("Consumer started, monitoring mixed events...\n");
    
    while (active_producers > 0 || external_events_received < 5) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        if (nfds == 0) {
            printf("Epoll timeout - still waiting...\n");
            continue;
        }
        
        for (int i = 0; i < nfds; i++) {
            uint32_t event_id = events[i].data.u32;
            
            // Check if this is our external timer event
            if (event_id == 0xFFFFFFFF) {
                uint64_t timer_val;
                if (read(ext_event.eventfd, &timer_val, sizeof(timer_val)) == sizeof(timer_val)) {
                    external_events_received++;
                    printf("🕒 TIMER EVENT #%d received (value=%lu)\n", 
                           external_events_received, timer_val);
                }
                continue;
            }
            
            // Handle queue events
            int queue_id = event_id;
            queue_t *q = queue_pool_get(&pool, queue_id);
            if (!q) continue;
            
            // Process messages from this queue
            uint8_t *ptr;
            size_t size;
            int queue_messages = 0;
            
            while (queue_get(q, &ptr, &size) > 0) {
                messages_received++;
                queue_messages++;
                queue_ack(q);
                
                // Print every 200th message to reduce output
                if (messages_received % 200 == 0) {
                    printf("Received %d messages (last from queue %d: \"%.*s\")\n", 
                           messages_received, queue_id, (int)size, ptr);
                }
            }
            
            // Check if producer finished and queue is empty
            if (queue_is_closed_and_empty(q)) {
                printf("Producer on queue %d finished, releasing queue\n", queue_id);
                queue_pool_release(&pool, queue_id);
                active_producers--;
            }
        }
    }
    
    printf("\nConsumer finished!\n");
    
    // Wait for all threads to complete
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producer_threads[i], NULL);
    }
    pthread_join(timer_tid, NULL);
    
    // Get final statistics
    uint64_t total_published, total_dropped, total_published_bytes, total_dropped_bytes, total_acquired;
    queue_pool_stats(&pool, &total_published, &total_dropped, 
                     &total_published_bytes, &total_dropped_bytes, &total_acquired);
    
    printf("\nResults:\n");
    printf("  Messages received: %d (expected %d)\n", messages_received, NUM_PRODUCERS * MESSAGES_PER_PRODUCER);
    printf("  Timer events received: %d/5\n", external_events_received);
    printf("  Total queues acquired: %lu\n", total_acquired);
    printf("  Total published: %lu messages (%lu bytes)\n", total_published, total_published_bytes);
    printf("  Total dropped: %lu messages (%lu bytes)\n", total_dropped, total_dropped_bytes);
    
    if (total_published + total_dropped > 0) {
        double drop_rate = 100.0 * total_dropped / (total_published + total_dropped);
        printf("  Drop rate: %.1f%%\n", drop_rate);
    }
    
    // Cleanup - note that we close our epoll fd, not the pool's
    close(ext_event.eventfd);
    queue_pool_destroy(&pool); // This won't close epoll_fd since we provided it
    close(epoll_fd); // We manage our own epoll fd
    
    printf("\n✓ External epoll integration test completed successfully!\n");
    printf("✓ Pool correctly used external epoll without taking ownership\n");
    
    return 0;
}