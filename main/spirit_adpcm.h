#pragma once
#include <stdint.h>
typedef struct { int predictor; int index; } spirit_adpcm_t;
int16_t spirit_adpcm_decode(spirit_adpcm_t *state, unsigned nibble);
