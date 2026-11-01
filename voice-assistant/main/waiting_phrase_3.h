/* Waiting phrase 3: "收到，请稍等", 16 kHz mono s16. */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PHRASE_3_SAMPLES[] = {
#include "waiting_phrase_3_p0.h"
#include "waiting_phrase_3_p1.h"
#include "waiting_phrase_3_p2.h"
#include "waiting_phrase_3_p3.h"
};

#define WAITING_PHRASE_3_COUNT 18762
