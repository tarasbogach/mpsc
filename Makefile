CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -pthread -mavx2 -mpopcnt
LDFLAGS = -pthread -lrt

BUILD_DIR = build
TARGET = $(BUILD_DIR)/mpsc
TEST_TARGET = $(BUILD_DIR)/test_concurrent
TEST_EPOLL_TARGET = $(BUILD_DIR)/test_external_epoll
TEST_REUSE_TARGET = $(BUILD_DIR)/test_pool_reuse
SOURCES = queue.c main.c
OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/%.o)
HEADERS = queue.h

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_TARGET): $(BUILD_DIR)/queue.o test_concurrent.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BUILD_DIR)/queue.o test_concurrent.c -o $(TEST_TARGET) $(LDFLAGS)

test_concurrent: $(TEST_TARGET)

$(TEST_EPOLL_TARGET): $(BUILD_DIR)/queue.o test_external_epoll.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BUILD_DIR)/queue.o test_external_epoll.c -o $(TEST_EPOLL_TARGET) $(LDFLAGS)

test_external_epoll: $(TEST_EPOLL_TARGET)

$(TEST_REUSE_TARGET): $(BUILD_DIR)/queue.o test_pool_reuse.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(BUILD_DIR)/queue.o test_pool_reuse.c -o $(TEST_REUSE_TARGET) $(LDFLAGS)

test_pool_reuse: $(TEST_REUSE_TARGET)

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET)
	$(TARGET)

run-slow: $(TARGET)
	$(TARGET) 5000

.PHONY: all clean run run-slow test_concurrent test_external_epoll test_pool_reuse
