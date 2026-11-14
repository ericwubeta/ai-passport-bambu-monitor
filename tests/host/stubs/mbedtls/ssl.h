#pragma once
#define MBEDTLS_SSL_MAX_FRAG_LEN_4096 4
int mbedtls_ssl_conf_max_frag_len(void *config, unsigned char length);
