#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "spirit_device_assets.h"
#include "spirit_voice_assets.h"

// Versioned read-only data in the existing spiffs partition. This is not a
// filesystem and is never formatted or written by the firmware.
void spirit_external_init(void);
bool spirit_external_attach(const void *data, size_t size);
bool spirit_external_ready(void);
size_t spirit_external_pack_bytes(void);
const spirit_device_clip_t *spirit_external_clip(unsigned role, unsigned family, unsigned action);
const spirit_voice_clip_t *spirit_external_voice(unsigned role, unsigned kind);
const char *spirit_external_voice_text(unsigned role, unsigned kind);
