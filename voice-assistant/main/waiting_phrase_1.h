/* Waiting phrase 1: "正在处理信息", 16 kHz mono s16. */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PHRASE_1_SAMPLES[] = {
#include "waiting_phrase_1_p0.h"
#include "waiting_phrase_1_p1.h"
#include "waiting_phrase_1_p2.h"
#include "waiting_phrase_1_p3.h"
};

#define WAITING_PHRASE_1_COUNT 22663
