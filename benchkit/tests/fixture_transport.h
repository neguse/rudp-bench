#ifndef BENCHKIT_TEST_FIXTURE_TRANSPORT_H
#define BENCHKIT_TEST_FIXTURE_TRANSPORT_H

#include "benchkit.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct fx_transport fx_transport;

fx_transport *fx_null_new(int n_conns);
fx_transport *fx_fault_inject_new(int n_conns, uint64_t delay_ns);
void fx_transport_free(fx_transport *t);

int fx_transport_send(fx_transport *t, const bk_header *h, uint64_t now_ns);
bool fx_transport_recv(fx_transport *t, int conn_index, uint64_t now_ns,
                       bk_header *out, uint64_t *recv_ts_ns);
uint64_t fx_transport_injected_duplicates(const fx_transport *t);

#endif  // BENCHKIT_TEST_FIXTURE_TRANSPORT_H
