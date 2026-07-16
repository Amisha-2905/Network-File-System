#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_CAPACITY 5

typedef struct CacheNode
{
    char path[256];
    int ss_id;
    struct CacheNode *prev;
    struct CacheNode *next;
} CacheNode;

typedef struct
{
    CacheNode *head;
    CacheNode *tail;
    int size;
} LRUCache;

static inline LRUCache *create_cache()
{
    LRUCache *cache = (LRUCache *)malloc(sizeof(LRUCache));
    cache->head = NULL;
    cache->tail = NULL;
    cache->size = 0;
    return cache;
}

// internal helper to move an entry to front of cache (most recently used)
static inline void move_to_head(LRUCache *cache, CacheNode *node)
{
    if (cache->head == node)
        return;

    // Detach from current position
    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;
    if (cache->tail == node)
        cache->tail = node->prev;

    // Attach to head position
    node->next = cache->head;
    node->prev = NULL;
    if (cache->head)
        cache->head->prev = node;
    cache->head = node;
    if (!cache->tail)
        cache->tail = node;
}

// Fetch routing paths inside cache boundary instantly
static inline int cache_get(LRUCache *cache, const char *path)
{
    CacheNode *curr = cache->head;
    while (curr != NULL)
    {
        if (strcmp(curr->path, path) == 0)
        {
            move_to_head(cache, curr);
            return curr->ss_id; // Cache Hit
        }
        curr = curr->next;
    }
    return -1; // Cache Miss
}

// Append entries, enforcing capacity eviction limits
static inline void cache_put(LRUCache *cache, const char *path, int ss_id)
{
    // Check if entry already exists to update it
    CacheNode *curr = cache->head;
    while (curr != NULL)
    {
        if (strcmp(curr->path, path) == 0)
        {
            curr->ss_id = ss_id;
            move_to_head(cache, curr);
            return;
        }
        curr = curr->next;
    }

    CacheNode *new_node = (CacheNode *)malloc(sizeof(CacheNode));
    strncpy(new_node->path, path, 256);
    new_node->ss_id = ss_id;
    new_node->prev = NULL;
    new_node->next = cache->head;

    if (cache->head)
        cache->head->prev = new_node;
    cache->head = new_node;
    if (!cache->tail)
        cache->tail = new_node;

    cache->size++;

    // Evict oldest record when capacity limit is breached
    if (cache->size > CACHE_CAPACITY)
    {
        CacheNode *oldest = cache->tail;
        cache->tail = oldest->prev;
        if (cache->tail)
            cache->tail->next = NULL;
        free(oldest);
        cache->size--;
    }
}

// Cache Invalidation for structural modification events
static inline void cache_invalidate(LRUCache *cache, const char *path)
{
    CacheNode *curr = cache->head;
    while (curr != NULL)
    {
        if (strcmp(curr->path, path) == 0)
        {
            if (curr->prev)
                curr->prev->next = curr->next;
            if (curr->next)
                curr->next->prev = curr->prev;
            if (cache->head == curr)
                cache->head = curr->next;
            if (cache->tail == curr)
                cache->tail = curr->prev;

            CacheNode *to_free = curr;
            curr = curr->next;
            free(to_free);
            cache->size--;
            return;
        }
        curr = curr->next;
    }
}

#endif