#define _DEFAULT_SOURCE
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define NUM_THREADS 8
#define ITERATIONS 100

typedef struct {
    queue_pool_t *mq;
    int thread_id;
    int *success_count;
    int *fail_count;
} thread_args_t;

// Thread that repeatedly adds and removes producers
void *stress_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    int local_success = 0;
    int local_fail = 0;
    
    for (int i = 0; i < ITERATIONS; i++) {
        // Try to add a producer
        int queue_id = queue_pool_acquire(args->mq);
        
        if (queue_id >= 0) {
            local_success++;
            
            // Send a message to verify it works
            queue_t *q = queue_pool_get(args->mq, queue_id);
            if (q) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Thread %d, iter %d", args->thread_id, i);
                queue_publish(q, msg, strlen(msg) + 1);
            }
            
            // Sleep a bit
            usleep(100);
            
            // Drain the queue to make it empty
            if (q) {
                uint8_t *ptr;
                size_t size;
                while (queue_get(q, &ptr, &size) > 0) {
                    queue_ack(q);
                }
                
                // Signal close
                queue_close(q);
            }
            
            // Try to remove it (should work since closed and empty)
            if (queue_pool_release(args->mq, queue_id) == 0) {
                // Success
            } else {
                printf("Thread %d: Failed to remove producer %d\n", args->thread_id, queue_id);
            }
        } else {
            local_fail++;
        }
        
        // Small random delay
        usleep(rand() % 100);
    }
    
    args->success_count[args->thread_id] = local_success;
    args->fail_count[args->thread_id] = local_fail;
    
    return NULL;
}

int main(void) {
    srand(time(NULL));
    
    printf("Concurrent add/remove stress test\n");
    printf("==================================\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Iterations per thread: %d\n\n", ITERATIONS);
    
    // Create multi-queue with limited capacity to force failures
    queue_pool_t mq;
    if (queue_pool_init(&mq, 16, 4096, -1) != 0) {
        fprintf(stderr, "Failed to initialize multi-queue\n");
        return 1;
    }
    
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    int success_counts[NUM_THREADS] = {0};
    int fail_counts[NUM_THREADS] = {0};
    
    // Create threads
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].mq = &mq;
        args[i].thread_id = i;
        args[i].success_count = success_counts;
        args[i].fail_count = fail_counts;
        
        if (pthread_create(&threads[i], NULL, stress_thread, &args[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }
    
    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Print results
    printf("\nResults:\n");
    int total_success = 0;
    int total_fail = 0;
    
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("  Thread %d: %d successful add/remove cycles, %d failed adds\n",
               i, success_counts[i], fail_counts[i]);
        total_success += success_counts[i];
        total_fail += fail_counts[i];
    }
    
    printf("\nTotal: %d successful cycles, %d failed adds\n", total_success, total_fail);
    printf("Active producers: %d, Free slots: %d\n", mq.num_active, mq.num_free);
    
    if (mq.num_active != 0) {
        printf("WARNING: %d producers still active (should be 0)\n", mq.num_active);
    }
    
    if (mq.num_free != mq.capacity) {
        printf("WARNING: Only %d free slots (should be %d)\n", mq.num_free, mq.capacity);
    }
    
    // Cleanup
    queue_pool_destroy(&mq);
    
    printf("\nStress test completed!\n");
    return 0;
}
