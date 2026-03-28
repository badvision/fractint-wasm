#ifndef WASM_KEY_H
#define WASM_KEY_H
#ifdef WASM_BUILD
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#define KEY_CHAN_SIZE 64

typedef struct {
    int ring[KEY_CHAN_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t  avail;
} KeyChannel;

extern KeyChannel menu_chan;
extern _Atomic int wasm_menu_active;

static inline void key_channel_init(KeyChannel *ch) {
    memset(ch->ring, 0, sizeof(ch->ring));
    ch->head = ch->tail = 0;
    pthread_mutex_init(&ch->lock, NULL);
    pthread_cond_init(&ch->avail, NULL);
}

static inline void key_channel_push(KeyChannel *ch, int key) {
    pthread_mutex_lock(&ch->lock);
    int next = (ch->head + 1) & (KEY_CHAN_SIZE - 1);
    if (next != ch->tail) {
        ch->ring[ch->head] = key;
        ch->head = next;
    }
    pthread_cond_signal(&ch->avail);
    pthread_mutex_unlock(&ch->lock);
}

static inline int key_channel_pop_blocking(KeyChannel *ch) {
    pthread_mutex_lock(&ch->lock);
    while (ch->head == ch->tail)
        pthread_cond_wait(&ch->avail, &ch->lock);
    int k = ch->ring[ch->tail];
    ch->tail = (ch->tail + 1) & (KEY_CHAN_SIZE - 1);
    pthread_mutex_unlock(&ch->lock);
    return k;
}

static inline int key_channel_pop_nowait(KeyChannel *ch) {
    pthread_mutex_lock(&ch->lock);
    if (ch->head == ch->tail) {
        pthread_mutex_unlock(&ch->lock);
        return 0;
    }
    int k = ch->ring[ch->tail];
    ch->tail = (ch->tail + 1) & (KEY_CHAN_SIZE - 1);
    pthread_mutex_unlock(&ch->lock);
    return k;
}
#endif /* WASM_BUILD */
#endif /* WASM_KEY_H */
