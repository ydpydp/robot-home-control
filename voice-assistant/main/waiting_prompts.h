/* Random waiting prompts (user decision 2026-10-25, option A):
   one of 5 Piper phrases, picked per round with esp_random(),
   played ONCE while /voice is pending. Upsampled x3 to the
   48 kHz stereo s32 bus by the waiting task in main.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#include "waiting_phrase_1.h"
#include "waiting_phrase_2.h"
#include "waiting_phrase_3.h"
#include "waiting_phrase_4.h"
#include "waiting_phrase_5.h"

static const int16_t *WAITING_PHRASES[] = {
    WAITING_PHRASE_1_SAMPLES,
    WAITING_PHRASE_2_SAMPLES,
    WAITING_PHRASE_3_SAMPLES,
    WAITING_PHRASE_4_SAMPLES,
    WAITING_PHRASE_5_SAMPLES
};

static const size_t WAITING_PHRASE_LENS[] = {
    WAITING_PHRASE_1_COUNT,
    WAITING_PHRASE_2_COUNT,
    WAITING_PHRASE_3_COUNT,
    WAITING_PHRASE_4_COUNT,
    WAITING_PHRASE_5_COUNT
};

#define WAITING_PHRASE_COUNT 5
