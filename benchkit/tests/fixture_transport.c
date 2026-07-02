#include "fixture_transport.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  bk_header h;
  uint64_t deliver_at_ns;
  uint64_t order;
} fx_packet;

typedef struct {
  fx_packet *items;
  size_t len;
  size_t cap;
} fx_queue;

struct fx_transport {
  int n_conns;
  bool inject_faults;
  uint64_t delay_ns;
  uint64_t next_order;
  uint64_t injected_duplicates;
  fx_queue *queues;
};

static int queue_push(fx_queue *q, const fx_packet *p) {
  if (q->len == q->cap) {
    const size_t next_cap = q->cap == 0 ? 16u : q->cap * 2u;
    fx_packet *next = (fx_packet *)realloc(q->items, next_cap * sizeof(*next));
    if (next == NULL) {
      return -1;
    }
    q->items = next;
    q->cap = next_cap;
  }
  q->items[q->len++] = *p;
  return 0;
}

static fx_transport *transport_new(int n_conns, bool inject_faults,
                                   uint64_t delay_ns) {
  if (n_conns <= 0) {
    return NULL;
  }
  fx_transport *t = (fx_transport *)calloc(1, sizeof(*t));
  if (t == NULL) {
    return NULL;
  }
  t->n_conns = n_conns;
  t->inject_faults = inject_faults;
  t->delay_ns = delay_ns;
  t->queues = (fx_queue *)calloc((size_t)n_conns, sizeof(*t->queues));
  if (t->queues == NULL) {
    fx_transport_free(t);
    return NULL;
  }
  return t;
}

fx_transport *fx_null_new(int n_conns) {
  return transport_new(n_conns, false, 0);
}

fx_transport *fx_fault_inject_new(int n_conns, uint64_t delay_ns) {
  return transport_new(n_conns, true, delay_ns);
}

void fx_transport_free(fx_transport *t) {
  if (t == NULL) {
    return;
  }
  if (t->queues != NULL) {
    for (int i = 0; i < t->n_conns; ++i) {
      free(t->queues[i].items);
    }
  }
  free(t->queues);
  free(t);
}

static int enqueue_one(fx_transport *t, int dest, const bk_header *h,
                       uint64_t now_ns, bool duplicate) {
  fx_packet p;
  memset(&p, 0, sizeof(p));
  p.h = *h;
  p.deliver_at_ns = now_ns;
  if (t->inject_faults && h->seq % 11u == 0u) {
    p.deliver_at_ns += t->delay_ns;
  }
  if (t->inject_faults && h->seq % 13u == 0u) {
    p.deliver_at_ns += t->delay_ns / 2u;
  }
  p.order = t->next_order++;
  if (duplicate) {
    p.order = t->next_order++;
  }
  return queue_push(&t->queues[dest], &p);
}

int fx_transport_send(fx_transport *t, const bk_header *h, uint64_t now_ns) {
  if (t == NULL || h == NULL || h->origin_id >= (uint32_t)t->n_conns) {
    return -1;
  }
  const bool broadcast = (h->flags & BK_FLAG_BROADCAST) != 0;
  const int first_dest = broadcast ? 0 : (int)h->origin_id;
  const int end_dest = broadcast ? t->n_conns : first_dest + 1;
  for (int dest = first_dest; dest < end_dest; ++dest) {
    if (enqueue_one(t, dest, h, now_ns, false) != 0) {
      return -1;
    }
    if (t->inject_faults && h->seq % 7u == 0u) {
      if (enqueue_one(t, dest, h, now_ns, true) != 0) {
        return -1;
      }
      t->injected_duplicates++;
    }
  }
  return 0;
}

bool fx_transport_recv(fx_transport *t, int conn_index, uint64_t now_ns,
                       bk_header *out, uint64_t *recv_ts_ns) {
  if (t == NULL || conn_index < 0 || conn_index >= t->n_conns || out == NULL ||
      recv_ts_ns == NULL) {
    return false;
  }
  fx_queue *q = &t->queues[conn_index];
  size_t best = q->len;
  for (size_t i = 0; i < q->len; ++i) {
    if (q->items[i].deliver_at_ns > now_ns) {
      continue;
    }
    if (best == q->len ||
        q->items[i].deliver_at_ns < q->items[best].deliver_at_ns ||
        (q->items[i].deliver_at_ns == q->items[best].deliver_at_ns &&
         q->items[i].order < q->items[best].order)) {
      best = i;
    }
  }
  if (best == q->len) {
    return false;
  }

  *out = q->items[best].h;
  *recv_ts_ns = q->items[best].deliver_at_ns;
  q->items[best] = q->items[q->len - 1u];
  q->len--;
  return true;
}

uint64_t fx_transport_injected_duplicates(const fx_transport *t) {
  return t == NULL ? 0 : t->injected_duplicates;
}
