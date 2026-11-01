/* Waiting prompt: Piper "正在分析你的信息", 16 kHz mono s16.
   Played on loop while /voice is pending. Upsampled x3 to the
   48 kHz stereo s32 bus by the waiting task in main.c.
   Samples are split across waiting_prompt_p*.h (tool arg limits). */
#pragma once
#include <stdint.h>

static const int16_t WAITING_PROMPT_SAMPLES[] = {
#include "waiting_prompt_p0.h"
#include "waiting_prompt_p1.h"
#include "waiting_prompt_p2.h"
#include "waiting_prompt_p3.h"
};

#define WAITING_PROMPT_COUNT 26936
