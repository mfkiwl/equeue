/*
 * Flexible event queue for dispatching events
 *
 * Copyright (c) 2016 Christopher Haster
 * Distributed under the MIT license
 */
#include "equeue.h"

#include <stdlib.h>
#include <string.h>


int equeue_create(equeue_t *q, size_t size) {
    void *buffer = malloc(size);
    if (!buffer) {
        return -1;
    }

    int err = equeue_create_inplace(q, size, buffer);
    q->allocated = buffer;
    return err;
}

int equeue_create_inplace(equeue_t *q, size_t size, void *buffer) {
    q->buffer = buffer;
    q->allocated = 0;

    q->npw2 = 0;
    for (unsigned s = size; s; s >>= 1) {
        q->npw2++;
    }

    q->chunks = 0;
    q->slab.size = size;
    q->slab.data = buffer;

    q->queue = 0;
    q->tick = equeue_tick();
    q->generation = 0;
    q->breaks = 0;

    q->background.active = false;
    q->background.update = 0;
    q->background.timer = 0;

    int err;
    err = equeue_sema_create(&q->eventsema);
    if (err < 0) {
        return err;
    }

    err = equeue_mutex_create(&q->queuelock);
    if (err < 0) {
        return err;
    }

    err = equeue_mutex_create(&q->memlock);
    if (err < 0) {
        return err;
    }

    return 0;
}

void equeue_destroy(equeue_t *q) {
    // call destructors on pending events
    for (struct equeue_event *es = q->queue; es; es = es->next) {
        for (struct equeue_event *e = q->queue; e; e = e->sibling) {
            if (e->dtor) {
                e->dtor(e + 1);
            }
        }
    }

    if (q->background.update) {
        q->background.update(q->background.timer, -1);
    }

    equeue_mutex_destroy(&q->memlock);
    equeue_mutex_destroy(&q->queuelock);
    equeue_sema_destroy(&q->eventsema);
    free(q->allocated);
}

// equeue chunk allocation functions
static struct equeue_event *equeue_mem_alloc(equeue_t *q, size_t size) {
    size += sizeof(struct equeue_event);
    size = (size + sizeof(void*)-1) & ~(sizeof(void*)-1);

    equeue_mutex_lock(&q->memlock);

    for (struct equeue_event **p = &q->chunks; *p; p = &(*p)->next) {
        if ((*p)->size >= size) {
            struct equeue_event *e = *p;
            if (e->sibling) {
                *p = e->sibling;
                (*p)->next = e->next;
            } else {
                *p = e->next;
            }

            equeue_mutex_unlock(&q->memlock);
            return e;
        }
    }

    if (q->slab.size >= size) {
        struct equeue_event *e = (struct equeue_event *)q->slab.data;
        q->slab.data += size;
        q->slab.size -= size;
        e->size = size;
        e->id = 1;

        equeue_mutex_unlock(&q->memlock);
        return e;
    }

    equeue_mutex_unlock(&q->memlock);
    return 0;
}

static void equeue_mem_dealloc(equeue_t *q, struct equeue_event *e) {
    equeue_mutex_lock(&q->memlock);

    struct equeue_event **p = &q->chunks;
    while (*p && (*p)->size < e->size) {
        p = &(*p)->next;
    }

    if (*p && (*p)->size == e->size) {
        e->sibling = *p;
        e->next = (*p)->next;
    } else {
        e->sibling = 0;
        e->next = *p;
    }
    *p = e;

    equeue_mutex_unlock(&q->memlock);
}

// equeue allocation functions
void *equeue_alloc(equeue_t *q, size_t size) {
    struct equeue_event *e = equeue_mem_alloc(q, size);
    if (!e) {
        return 0;
    }

    e->target = 0;
    e->period = -1;
    e->dtor = 0;

    return e + 1;
}

void equeue_dealloc(equeue_t *q, void *p) {
    struct equeue_event *e = (struct equeue_event*)p - 1;

    if (e->dtor) {
        e->dtor(e+1);
    }

    equeue_mem_dealloc(q, e);
}

// equeue scheduling functions
static inline int equeue_tickdiff(unsigned a, unsigned b) {
    return (int)(a - b);
}

static inline int equeue_clampdiff(unsigned a, unsigned b) {
    int diff = equeue_tickdiff(a, b);
    return ~(diff >> (8*sizeof(int)-1)) & diff;
}

static inline void equeue_incid(equeue_t *q, struct equeue_event *e) {
    e->id += 1;
    if (e->id >> (8*sizeof(int)-1 - q->npw2)) {
        e->id = 1;
    }
}

static int equeue_enqueue(equeue_t *q, struct equeue_event *e, unsigned ms) {
    int id = (e->id << q->npw2) | ((unsigned char *)e - q->buffer);
    e->target = equeue_tick() + ms;
    e->generation = q->generation;

    equeue_mutex_lock(&q->queuelock);

    struct equeue_event **p = &q->queue;
    while (*p && equeue_tickdiff((*p)->target, e->target) < 0) {
        p = &(*p)->next;
    }

    if (*p && (*p)->target == e->target) {
        e->next = (*p)->next;
        if (e->next) {
            e->next->ref = &e->next;
        }

        e->sibling = *p;
        e->sibling->ref = &e->sibling;
    } else {
        e->next = *p;
        if (e->next) {
            e->next->ref = &e->next;
        }

        e->sibling = 0;
    }

    *p = e;
    e->ref = p;

    if ((q->background.update && q->background.active) &&
        (q->queue == e && !e->sibling)) {
        q->background.update(q->background.timer, ms);
    }

    equeue_mutex_unlock(&q->queuelock);

    return id;
}

static struct equeue_event *equeue_unqueue(equeue_t *q, int id) {
    struct equeue_event *e = (struct equeue_event *)
            &q->buffer[id & ((1 << q->npw2)-1)];

    equeue_mutex_lock(&q->queuelock);
    if (e->id != id >> q->npw2) {
        equeue_mutex_unlock(&q->queuelock);
        return 0;
    }

    e->cb = 0;
    e->period = -1;

    int diff = equeue_tickdiff(e->target, q->tick);
    if (diff < 0 || (diff == 0 && e->generation != q->generation)) {
        equeue_mutex_unlock(&q->queuelock);
        return 0;
    }

    if (e->sibling) {
        e->sibling->next = e->next;
        if (e->sibling->next) {
            e->sibling->next->ref = &e->sibling->next;
        }

        *e->ref = e->sibling;
        e->sibling->ref = e->ref;
    } else {
        *e->ref = e->next;
        if (e->next) {
            e->next->ref = e->ref;
        }
    }

    equeue_incid(q, e);
    equeue_mutex_unlock(&q->queuelock);

    return e;
}

static struct equeue_event *equeue_dequeue(equeue_t *q, unsigned target) {
    equeue_mutex_lock(&q->queuelock);

    q->generation += 1;
    if (equeue_tickdiff(q->tick, target) <= 0) {
        q->tick = target;
    }

    struct equeue_event *head = q->queue;
    struct equeue_event **p = &head;
    while (*p && equeue_tickdiff((*p)->target, target) <= 0) {
        p = &(*p)->next;
    }

    q->queue = *p;
    if (q->queue) {
        q->queue->ref = &q->queue;
    }

    *p = 0;

    equeue_mutex_unlock(&q->queuelock);

    struct equeue_event **tail = &head;
    struct equeue_event *ess = head;
    while (ess) {
        struct equeue_event *es = ess;
        ess = es->next;

        struct equeue_event *prev = 0;
        for (struct equeue_event *e = es; e; e = e->sibling) {
            e->next = prev;
            prev = e;
        }

        *tail = prev;
        tail = &es->next;
    }

    return head;
}

int equeue_post(equeue_t *q, void (*cb)(void*), void *p) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->cb = cb;
    e->target = equeue_clampdiff(e->target, 0);

    int id = equeue_enqueue(q, e, e->target);
    equeue_sema_signal(&q->eventsema);
    return id;
}

void equeue_cancel(equeue_t *q, int id) {
    struct equeue_event *e = equeue_unqueue(q, id);
    if (e) {
        equeue_dealloc(q, e + 1);
    }
}

void equeue_break(equeue_t *q) {
    equeue_mutex_lock(&q->queuelock);
    q->breaks++;
    equeue_mutex_unlock(&q->queuelock);
    equeue_sema_signal(&q->eventsema);
}

void equeue_dispatch(equeue_t *q, int ms) {
    unsigned tick = equeue_tick();
    unsigned timeout = tick + ms;
    q->background.active = false;

    while (1) {
        // collect all the available events and next deadline
        struct equeue_event *es = equeue_dequeue(q, tick);

        // dispatch events
        while (es) {
            struct equeue_event *e = es;
            es = e->next;

            // actually dispatch the callbacks
            void (*cb)(void *) = e->cb;
            if (cb) {
                cb(e + 1);
            }

            // reenqueue periodic events or deallocate
            if (e->period >= 0) {
                equeue_enqueue(q, e, e->period);
            } else {
                equeue_incid(q, e);
                equeue_dealloc(q, e+1);
            }
        }

        int deadline = -1;
        tick = equeue_tick();

        // check if we should stop dispatching soon
        if (ms >= 0) {
            deadline = equeue_tickdiff(timeout, tick);
            if (deadline <= 0) {
                // update background timer if necessary
                if (q->background.update) {
                    equeue_mutex_lock(&q->queuelock);
                    if (q->background.update && q->queue) {
                        q->background.update(q->background.timer,
                                equeue_clampdiff(q->queue->target, tick));
                    }
                    q->background.active = true;
                    equeue_mutex_unlock(&q->queuelock);
                }
                return;
            }
        }

        // find closest deadline
        equeue_mutex_lock(&q->queuelock);
        if (q->queue) {
            int diff = equeue_clampdiff(q->queue->target, tick);
            if ((unsigned)diff < (unsigned)deadline) {
                deadline = diff;
            }
        }
        equeue_mutex_unlock(&q->queuelock);

        // wait for events
        equeue_sema_wait(&q->eventsema, deadline);

        // check if we were notified to break out of dispatch
        if (q->breaks) {
            equeue_mutex_lock(&q->queuelock);
            if (q->breaks > 0) {
                q->breaks--;
                equeue_mutex_unlock(&q->queuelock);
                return;
            }
            equeue_mutex_unlock(&q->queuelock);
        }

        // update tick for next iteration
        tick = equeue_tick();
    }
}

// event functions
void equeue_event_delay(void *p, int ms) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->target = ms;
}

void equeue_event_period(void *p, int ms) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->period = ms;
}

void equeue_event_dtor(void *p, void (*dtor)(void *)) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->dtor = dtor;
}

// simple callbacks 
struct ecallback {
    void (*cb)(void*);
    void *data;
};

static void ecallback_dispatch(void *p) {
    struct ecallback *e = (struct ecallback*)p;
    e->cb(e->data);
}

int equeue_call(equeue_t *q, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}

int equeue_call_in(equeue_t *q, int ms, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    equeue_event_delay(e, ms);
    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}

int equeue_call_every(equeue_t *q, int ms, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    equeue_event_delay(e, ms);
    equeue_event_period(e, ms);
    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}

// backgrounding
void equeue_background(equeue_t *q,
        void (*update)(void *timer, int ms), void *timer) {
    equeue_mutex_lock(&q->queuelock);
    if (q->background.update) {
        q->background.update(q->background.timer, -1);
    }

    q->background.update = update;
    q->background.timer = timer;

    if (q->background.update && q->queue) {
        q->background.update(q->background.timer,
                equeue_clampdiff(q->queue->target, equeue_tick()));
    }
    q->background.active = true;
    equeue_mutex_unlock(&q->queuelock);
}

struct equeue_chain_context {
    equeue_t *q;
    equeue_t *target;
    int id;
};

static void equeue_chain_dispatch(void *p) {
    equeue_dispatch((equeue_t *)p, 0);
}

static void equeue_chain_update(void *p, int ms) {
    struct equeue_chain_context *c = (struct equeue_chain_context *)p;
    equeue_cancel(c->target, c->id);

    if (ms >= 0) {
        c->id = equeue_call_in(c->target, ms, equeue_chain_dispatch, c->q);
    } else {
        equeue_dealloc(c->target, c);
    }
}

void equeue_chain(equeue_t *q, equeue_t *target) {
    struct equeue_chain_context *c = equeue_alloc(q,
            sizeof(struct equeue_chain_context));

    c->q = q;
    c->target = target;
    c->id = 0;

    equeue_background(q, equeue_chain_update, c);
}
