#ifndef RUDP_BENCH_LSQUIC_CERT_HELPER_H
#define RUDP_BENCH_LSQUIC_CERT_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

struct ssl_ctx_st;

struct ssl_ctx_st* rudp_bench_lsquic_create_ssl_ctx(const char* cert_path,
                                                     const char* key_path);

#ifdef __cplusplus
}
#endif

#endif
