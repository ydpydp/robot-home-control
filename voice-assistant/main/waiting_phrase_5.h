/* Waiting phrase 5: "正在准备答案", 16 kHz mono s16. */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PHRASE_5_SAMPLES[] = {
#include "waiting_phrase_5_p0.h"
#include "waiting_phrase_5_p1.h"
#include "waiting_phrase_5_p2.h"
#include "waiting_phrase_5_p3.h"
#include "waiting_phrase_5_p4.h"
};

#define WAITING_PHRASE_5_COUNT 24892
