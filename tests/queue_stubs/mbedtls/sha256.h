#pragma once
#include <stddef.h>
// Policy-test double only; firmware uses the real ESP-IDF mbedTLS implementation.
int mbedtls_sha256(const unsigned char *data,size_t length,unsigned char output[32],int is224);
