#include "cache.h"
#include "map.h"
#include <sys/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>

typedef struct cache_entry_s {
    char *key;
    void *value;
    int   size;
    void (*evict_cb)(void *);

    TAILQ_ENTRY(cache_entry_s) entry;
} cache_entry_t;

struct cache_s {
    int total_size;
    int used_size;

    pthread_mutex_t cache_lock;

    TAILQ_HEAD(entries_head, cache_entry_s) entries;
    map_t *map;
};

cache_t *cache_init(int cache_size) {
    cache_t *cache = (cache_t *)malloc(sizeof(cache_t));
    bzero(cache, sizeof(cache_t));

    cache->total_size = cache_size;
    TAILQ_INIT(&cache->entries);

    pthread_mutex_init(&cache->cache_lock, NULL);
    cache->map = map_init();

    return cache;
}

int cache_insert(cache_t *cache, char *key, void *val, int size, void (*evict_cb)(void *)) {

    TAILQ_HEAD(entries_head, cache_entry_s) evict_ents;
    TAILQ_INIT(&evict_ents);

    pthread_mutex_lock(&cache->cache_lock);
    // Check if this entry already exists in the map:
    if (map_get(cache->map, key)) {
        // Entry already exists..
        pthread_mutex_unlock(&cache->cache_lock);
        return EEXIST;
    }

    cache_entry_t *ent = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (ent == NULL) {
        return ENOMEM;
    }

    bzero(ent, sizeof(cache_entry_t));

    ent->key = key;
    ent->value = val;
    ent->size = size;
    ent->evict_cb = evict_cb;

    // Make room for this entry in the cache:
    int free_space = cache->total_size - cache->used_size;
    int space_needed = size - free_space;
    while (space_needed > 0) {
        cache_entry_t *lru_ent = TAILQ_LAST(&cache->entries, entries_head);
        space_needed -= lru_ent->size;
        TAILQ_REMOVE(&cache->entries, lru_ent, entry);
        map_delete(cache->map, lru_ent->key);
        cache->used_size -= lru_ent->size;
        TAILQ_INSERT_HEAD(&evict_ents, lru_ent, entry);
    }

    TAILQ_INSERT_TAIL(&cache->entries, ent, entry);
    map_put(cache->map, key, ent);
    cache->used_size += size;
    pthread_mutex_unlock(&cache->cache_lock);

    while (!TAILQ_EMPTY(&evict_ents)) {
        cache_entry_t *ent = TAILQ_FIRST(&evict_ents);
        TAILQ_REMOVE(&evict_ents, ent, entry);

        if (ent->evict_cb) {
            ent->evict_cb(ent->value);
        }

        free(ent);
    }

    return 0;
}


int cache_get(cache_t *cache, char *key, void **val) {

    pthread_mutex_lock(&cache->cache_lock);

    cache_entry_t *ent = map_get(cache->map, key);

    if (ent == NULL) {
        pthread_mutex_unlock(&cache->cache_lock);
        return ENOENT;
    }

    *val = ent->value;

    TAILQ_REMOVE(&cache->entries, ent, entry);
    TAILQ_INSERT_HEAD(&cache->entries, ent, entry);

    pthread_mutex_unlock(&cache->cache_lock);

    return 0;
}

int cache_evict(cache_t *cache, char *key) {
    pthread_mutex_lock(&cache->cache_lock);

    cache_entry_t *ent = map_get(cache->map, key);

    if (ent == NULL) {
        pthread_mutex_unlock(&cache->cache_lock);
        return ENOENT;
    }

    TAILQ_REMOVE(&cache->entries, ent, entry);
    map_delete(cache->map, ent->key);
    cache->used_size -= ent->size;
    pthread_mutex_unlock(&cache->cache_lock);

    if (ent->evict_cb) {
        ent->evict_cb(ent->value);
    }

    free(ent);
    return 0;
}

void cache_free(cache_t *cache) {
    if (cache == NULL) {
        return;
    }

    pthread_mutex_destroy(&cache->cache_lock);

    map_free(cache->map);
    while (!TAILQ_EMPTY(&cache->entries)) {
        cache_entry_t *ent = TAILQ_FIRST(&cache->entries);
        TAILQ_REMOVE(&cache->entries, ent, entry);

        if (ent->evict_cb) {
            ent->evict_cb(ent->value);
        }

        free(ent);
    }

    free(cache);
}