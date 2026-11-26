#pragma once
#include <stddef.h>
#include <stdint.h>
// Incremental policy-test double; firmware uses real mbedTLS SHA-256.
typedef struct { uint32_t value; uint64_t bytes; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *context);
void mbedtls_sha256_free(mbedtls_sha256_context *context);
int mbedtls_sha256_starts(mbedtls_sha256_context *context,int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *context,const unsigned char *data,size_t length);
int mbedtls_sha256_finish(mbedtls_sha256_context *context,unsigned char output[32]);
int mbedtls_sha256(const unsigned char *data,size_t length,unsigned char output[32],int is224);
