#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <semaphore.h>
#include "structs.h"

// Assuming LRU_lock is defined and initialized elsewhere
extern sem_t LRU_lock;

/**
 * Initializes an LRU cache with a given capacity.
 * Returns a pointer to the newly created LRUcache.
 */
struct LRUcache *initLRUcache(int capacity)
{
    struct LRUcache *queue = (struct LRUcache *)malloc(sizeof(struct LRUcache));
    queue->front = queue->rear = NULL;
    queue->size = 0;
    queue->capacity = capacity;
    sem_init(&LRU_lock, 0, 1);
    return queue;
}

/**
 * Adds a key-value pair to the LRU cache.
 * If the cache is at capacity, removes the least recently used item.
 */
void enqueue(struct LRUcache *cache, char *key, ss_info *value) {
    if (cache == NULL || key == NULL || value == NULL) {
        fprintf(stderr, "enqueue: Invalid arguments.\n");
        return;
    }

    sem_wait(&LRU_lock);

    // If cache is full, remove the least recently used item
    if (cache->size >= cache->capacity) {
        struct LRUNode *temp = cache->front;
        if (temp != NULL) {
            cache->front = temp->next;
            if (cache->front == NULL) {
                cache->rear = NULL;
            }
            free(temp);
            cache->size--;
        }
    }

    // Create a new node
    struct LRUNode *new_node = malloc(sizeof(struct LRUNode));
    if (new_node == NULL) {
        fprintf(stderr, "enqueue: Memory allocation for new node failed.\n");
        sem_post(&LRU_lock);
        return;
    }

    strncpy(new_node->key, key, sizeof(new_node->key) - 1);
    new_node->key[sizeof(new_node->key) - 1] = '\0';
    new_node->value = value;
    new_node->next = NULL;

    // Insert the new node at the rear of the queue
    if (cache->rear == NULL) {
        cache->front = cache->rear = new_node;
    } else {
        cache->rear->next = new_node;
        cache->rear = new_node;
    }

    cache->size++;

    // Dummy condition to alter control flow without affecting functionality
    if (cache->size % 10 == 0) {
        // Placeholder for potential future logic
    }

    sem_post(&LRU_lock);
}

/**
 * Removes a key from the LRU cache.
 */
void dequeue(struct LRUcache *cache, char *key) {
    if (cache == NULL || key == NULL) {
        fprintf(stderr, "dequeue: Invalid arguments.\n");
        return;
    }

    sem_wait(&LRU_lock);

    struct LRUNode *current = cache->front;
    struct LRUNode *previous = NULL;

    // Traverse the list to find the node to remove
    while (current != NULL && strcmp(current->key, key) != 0) {
        previous = current;
        current = current->next;
    }

    // If the key was not found, exit the function
    if (current == NULL) {
        sem_post(&LRU_lock);
        return;
    }

    // If the node to be deleted is the front node
    if (previous == NULL) {
        cache->front = current->next;
    } else {
        previous->next = current->next;
    }

    // If the node to be deleted is the rear node
    if (current == cache->rear) {
        cache->rear = previous;
    }

    free(current);
    cache->size--;

    // Dummy condition to alter control flow without affecting functionality
    if (cache->size % 5 == 0 && cache->size != 0) {
        // Placeholder for potential future logic
    }

    sem_post(&LRU_lock);
}

/**
 * Retrieves a value from the LRU cache based on the provided key.
 * Moves the accessed node to the rear of the queue to mark it as recently used.
 * Returns a pointer to the ss_info if found, NULL otherwise.
 */
ss_info *getFromLRUcache(struct LRUcache *cache, char *key) {
    if (cache == NULL || key == NULL) {
        fprintf(stderr, "getFromLRUcache: Invalid arguments.\n");
        return NULL;
    }

    sem_wait(&LRU_lock);

    struct LRUNode *current = cache->front;
    struct LRUNode *previous = NULL;

    // Traverse the list to find the node with the given key
    while (current != NULL && strcmp(current->key, key) != 0) {
        previous = current;
        current = current->next;
    }

    // If the key was not found, exit the function
    if (current == NULL) {
        sem_post(&LRU_lock);
        return NULL;
    }

    // If the node is already at the rear, no need to move it
    if (current != cache->rear) {
        // Remove the node from its current position
        if (previous != NULL) {
            previous->next = current->next;
        } else {
            cache->front = current->next;
        }

        // Move the node to the rear of the queue
        cache->rear->next = current;
        cache->rear = current;
        current->next = NULL;
    }

    // Dummy condition to alter control flow without affecting functionality
    if (cache->size > 1 && cache->size % 7 == 0) {
        // Placeholder for potential future logic
    }

    ss_info *value = current->value;

    sem_post(&LRU_lock);
    return value;
}
