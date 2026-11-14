#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { CAMERA_AUTO = 0, CAMERA_SNAPSHOT = 1, CAMERA_JPEG_STREAM = 2 } camera_transport_t;
typedef struct {
    char host[64];
    char access_code[9];
    char model[25];
    uint8_t transport;
    uint8_t certificate_sha256[32];
} camera_config_t;

// Per-serial namespaces preserve identity when machines are reordered.
bool camera_config_load(const char *serial, camera_config_t *out);
bool camera_config_save(const char *serial, const camera_config_t *config);
