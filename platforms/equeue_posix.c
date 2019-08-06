/*
 * Implementation for Posix compliant platforms
 *
 * Copyright (c) 2016 Christopher Haster
 * Distributed under the MIT license
 */
#include "equeue_platform.h"

#if defined(EQUEUE_PLATFORM_POSIX) && !defined(EQUEUE_PLATFORM)

#include <time.h>
#include <sys/time.h>
#include <errno.h>


// Tick operations
equeue_tick_t equeue_tick(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (equeue_tick_t)(tv.tv_sec*1000 + tv.tv_usec/1000);
}


// Mutex operations
int equeue_mutex_create(equeue_mutex_t *m) {
    return -pthread_mutex_init(m, 0);
}

void equeue_mutex_destroy(equeue_mutex_t *m) {
    pthread_mutex_destroy(m);
}

void equeue_mutex_lock(equeue_mutex_t *m) {
    pthread_mutex_lock(m);
}

void equeue_mutex_unlock(equeue_mutex_t *m) {
    pthread_mutex_unlock(m);
}


// Semaphore operations
int equeue_sema_create(equeue_sema_t *s) {
    int err = pthread_mutex_init(&s->mutex, 0);
    if (err) {
        return -err;
    }

    err = pthread_cond_init(&s->cond, 0);
    if (err) {
        return -err;
    }

    s->signal = false;
    return 0;
}

void equeue_sema_destroy(equeue_sema_t *s) {
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
}

void equeue_sema_signal(equeue_sema_t *s) {
    pthread_mutex_lock(&s->mutex);
    s->signal = true;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}

int equeue_sema_wait(equeue_sema_t *s, int ms) {
    pthread_mutex_lock(&s->mutex);
    if (!s->signal) {
        if (ms < 0) {
            pthread_cond_wait(&s->cond, &s->mutex);
        } else {
            struct timeval tv;
            gettimeofday(&tv, 0);

            struct timespec ts = {
                .tv_sec = ms/1000 + tv.tv_sec,
                .tv_nsec = ms*1000000 + tv.tv_usec*1000,
            };

            pthread_cond_timedwait(&s->cond, &s->mutex, &ts);
        }
    }

    bool signal = s->signal;
    s->signal = false;
    pthread_mutex_unlock(&s->mutex);

    return signal ? 0 : EQUEUE_ERR_TIMEDOUT;
}

#endif