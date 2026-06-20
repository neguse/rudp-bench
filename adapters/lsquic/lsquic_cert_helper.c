#include "lsquic_cert_helper.h"

#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <stdlib.h>
#include <string.h>

static int select_alpn(SSL *ssl, const unsigned char **out,
                       unsigned char *outlen, const unsigned char *in,
                       unsigned int inlen, void *arg) {
  (void)ssl;
  const unsigned char *alpn = (const unsigned char *)arg;
  unsigned char alpn_len = alpn[0];
  const unsigned char *alpn_proto = alpn + 1;

  const unsigned char *p = in;
  const unsigned char *end = in + inlen;
  while (p < end) {
    unsigned char len = *p++;
    if (p + len > end) break;
    if (len == alpn_len && memcmp(p, alpn_proto, len) == 0) {
      *out = p;
      *outlen = len;
      return SSL_TLSEXT_ERR_OK;
    }
    p += len;
  }
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* Wire-format ALPN: length-prefixed "rudp-bnch" */
static const unsigned char rudp_alpn[] = "\x09rudp-bnch";

struct ssl_ctx_st* rudp_bench_lsquic_create_ssl_ctx(const char* cert_path,
                                                     const char* key_path) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_method());
  if (!ctx) abort();

  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

  if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
    SSL_CTX_free(ctx);
    abort();
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(ctx);
    abort();
  }

  SSL_CTX_set_alpn_select_cb(ctx, select_alpn, (void *)rudp_alpn);

  return ctx;
}
