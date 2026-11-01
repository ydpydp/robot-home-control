/* Waiting phrase 4: "大模型正在思考", 16 kHz mono s16. */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PHRASE_4_SAMPLES[] = {
#include "waiting_phrase_4_p0.h"
#include "waiting_phrase_4_p1.h"
#include "waiting_phrase_4_p2.h"
#include "waiting_phrase_4_p3.h"
#include "waiting_phrase_4_p4.h"
#include "waiting_phrase_4_p5.h"
};

#define WAITING_PHRASE_4_COUNT 30837
