#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POOL_SIZE 4
#define QUEUE_CAPACITY 4096
#define TEST_MSG "Test message"

int main() {
    printf("Pool Exhaustion and Reuse Test\n");
    printf("===============================\n");
    printf("Pool size: %d queues\n\n", POOL_SIZE);

    queue_pool_t pool;
    if (queue_pool_init(&pool, POOL_SIZE, QUEUE_CAPACITY, -1) != 0) {
        fprintf(stderr, "Failed to initialize queue pool\n");
        return 1;
    }

    printf("Phase 1: Exhaust the pool\n");
    printf("-------------------------\n");

    int queue_ids[POOL_SIZE + 1];

    // Acquire all available queues
    for (int i = 0; i < POOL_SIZE; i++) {
        queue_ids[i] = queue_pool_acquire(&pool);
        if (queue_ids[i] == -1) {
            fprintf(stderr, "✗ Failed to acquire queue %d (unexpected)\n", i);
            queue_pool_destroy(&pool);
            return 1;
        }
        printf("✓ Acquired queue %d (id=%d), free slots remaining: %d\n",
               i, queue_ids[i], POOL_SIZE - i - 1);
    }

    // Try to acquire one more (should fail)
    printf("\nTrying to acquire from exhausted pool...\n");
    int extra_id = queue_pool_acquire(&pool);
    if (extra_id == -1) {
        printf("✓ Correctly rejected acquisition from full pool\n");
    } else {
        fprintf(stderr, "✗ Should have failed to acquire from full pool, got id=%d\n", extra_id);
        queue_pool_destroy(&pool);
        return 1;
    }

    printf("\nPhase 2: Send messages to all queues\n");
    printf("-------------------------------------\n");

    // Send a few messages to each queue
    for (int i = 0; i < POOL_SIZE; i++) {
        queue_t *q = queue_pool_get(&pool, queue_ids[i]);
        if (!q) {
            fprintf(stderr, "✗ Failed to get queue %d\n", queue_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }

        char msg[64];
        for (int j = 0; j < 3; j++) {
            snprintf(msg, sizeof(msg), "Queue %d Message %d", queue_ids[i], j);
            if (!queue_publish(q, msg, strlen(msg))) {
                fprintf(stderr, "✗ Failed to publish to queue %d\n", queue_ids[i]);
                queue_pool_destroy(&pool);
                return 1;
            }
        }
        printf("✓ Sent 3 messages to queue %d\n", queue_ids[i]);
    }

    printf("\nPhase 3: Close and release half the queues\n");
    printf("-------------------------------------------\n");

    // Close first two queues
    for (int i = 0; i < POOL_SIZE / 2; i++) {
        queue_t *q = queue_pool_get(&pool, queue_ids[i]);
        queue_close(q);

        // Drain the queue
        uint8_t *ptr;
        size_t size;
        int msg_count = 0;
        while (queue_get(q, &ptr, &size) > 0) {
            queue_ack(q);
            msg_count++;
        }

        // Release the queue
        if (queue_pool_release(&pool, queue_ids[i]) != 0) {
            fprintf(stderr, "✗ Failed to release queue %d\n", queue_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }
        printf("✓ Released queue %d (drained %d messages)\n", queue_ids[i], msg_count);
    }

    printf("\nPhase 4: Reuse released slots\n");
    printf("------------------------------\n");

    // Acquire new queues (should reuse the freed slots)
    int reused_ids[POOL_SIZE / 2];
    for (int i = 0; i < POOL_SIZE / 2; i++) {
        reused_ids[i] = queue_pool_acquire(&pool);
        if (reused_ids[i] == -1) {
            fprintf(stderr, "✗ Failed to reacquire queue %d\n", i);
            queue_pool_destroy(&pool);
            return 1;
        }

        // Check if we got one of the previously released IDs
        int is_reused = 0;
        for (int j = 0; j < POOL_SIZE / 2; j++) {
            if (reused_ids[i] == queue_ids[j]) {
                is_reused = 1;
                break;
            }
        }

        printf("✓ Acquired queue %d (id=%d) %s\n",
               i, reused_ids[i], is_reused ? "[REUSED slot]" : "[NEW slot]");
    }

    printf("\nPhase 5: Verify reused queues work correctly\n");
    printf("---------------------------------------------\n");

    // Send and receive messages on reused queues
    for (int i = 0; i < POOL_SIZE / 2; i++) {
        queue_t *q = queue_pool_get(&pool, reused_ids[i]);
        if (!q) {
            fprintf(stderr, "✗ Failed to get reused queue %d\n", reused_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }

        // Send messages
        char send_msg[64];
        snprintf(send_msg, sizeof(send_msg), "Reused Queue %d Test", reused_ids[i]);
        if (!queue_publish(q, send_msg, strlen(send_msg))) {
            fprintf(stderr, "✗ Failed to publish to reused queue %d\n", reused_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }

        // Receive and verify
        uint8_t *ptr;
        size_t size;
        if (queue_get(q, &ptr, &size) == 0) {
            fprintf(stderr, "✗ Failed to get message from reused queue %d\n", reused_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }

        if (size != strlen(send_msg) || memcmp(ptr, send_msg, size) != 0) {
            fprintf(stderr, "✗ Message mismatch on reused queue %d\n", reused_ids[i]);
            queue_pool_destroy(&pool);
            return 1;
        }

        queue_ack(q);
        printf("✓ Queue %d works correctly (sent and received message)\n", reused_ids[i]);
    }

    printf("\nPhase 6: Try to exhaust pool again\n");
    printf("-----------------------------------\n");

    // We should still have POOL_SIZE/2 original queues + POOL_SIZE/2 reused queues = full
    int another_id = queue_pool_acquire(&pool);
    if (another_id == -1) {
        printf("✓ Pool is full again (cannot acquire more)\n");
    } else {
        fprintf(stderr, "✗ Pool should be full, but acquired id=%d\n", another_id);
        queue_pool_destroy(&pool);
        return 1;
    }

    printf("\nPhase 7: Cleanup all queues\n");
    printf("----------------------------\n");

    // Close and release all remaining queues
    for (int i = POOL_SIZE / 2; i < POOL_SIZE; i++) {
        queue_t *q = queue_pool_get(&pool, queue_ids[i]);
        queue_close(q);

        uint8_t *ptr;
        size_t size;
        while (queue_get(q, &ptr, &size) > 0) {
            queue_ack(q);
        }

        queue_pool_release(&pool, queue_ids[i]);
    }

    for (int i = 0; i < POOL_SIZE / 2; i++) {
        queue_t *q = queue_pool_get(&pool, reused_ids[i]);
        queue_close(q);

        uint8_t *ptr;
        size_t size;
        while (queue_get(q, &ptr, &size) > 0) {
            queue_ack(q);
        }

        queue_pool_release(&pool, reused_ids[i]);
    }

    printf("✓ All queues released\n");

    // Get final statistics
    uint64_t total_published, total_dropped, total_pub_bytes, total_drop_bytes, total_acquired;
    queue_pool_stats(&pool, &total_published, &total_dropped,
                     &total_pub_bytes, &total_drop_bytes, &total_acquired);

    printf("\nFinal Statistics:\n");
    printf("  Total queues acquired (lifetime): %lu\n", total_acquired);
    printf("  Total messages published: %lu\n", total_published);
    printf("  Pool size: %d\n", POOL_SIZE);
    printf("  Expected acquisitions: %d (initial) + %d (reused) = %d\n",
           POOL_SIZE, POOL_SIZE / 2, POOL_SIZE + POOL_SIZE / 2);

    if (total_acquired == POOL_SIZE + POOL_SIZE / 2) {
        printf("\n✓ REUSE TEST PASSED: Bitmap correctly tracked %lu acquisitions!\n", total_acquired);
    } else {
        fprintf(stderr, "\n✗ REUSE TEST FAILED: Expected %d acquisitions, got %lu\n",
                POOL_SIZE + POOL_SIZE / 2, total_acquired);
        queue_pool_destroy(&pool);
        return 1;
    }

    queue_pool_destroy(&pool);

    printf("\n✓ Pool exhaustion and reuse test completed successfully!\n");
    return 0;
}
