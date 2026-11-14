#pragma once
#include <stdint.h>
// rate == 0 keeps the historical 12kHz behaviour; newer clips set it explicitly.
typedef struct { const uint8_t *data; uint32_t samples; uint16_t rate; } spirit_voice_clip_t;
extern const spirit_voice_clip_t spirit_voices[4][5];
extern const spirit_voice_clip_t spirit_page_voices[4][3][2];
