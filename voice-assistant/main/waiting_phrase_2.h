/* Waiting phrase 2: "收到", 16 kHz mono s16. */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PHRASE_2_SAMPLES[] = {
#include "waiting_phrase_2_p0.h"
#include "waiting_phrase_2_p1.h"
#include "waiting_phrase_2_p2.h"
};

#define WAITING_PHRASE_2_COUNT 12261
