/* Conversational robot v1 firmware -- FRESH ESP-IDF build, NOT the Muse
 * gadget route. Board: N16R8 (YD-ESP32-S3-COREBOARD V1.4).
 *
 * Wiring (verified on N16R8, from the gadget phase):
 *   INMP441 mic:  SCK->GPIO4, WS->GPIO5, SD->GPIO7, L/R->3.3V (right slot)
 *   MAX98357A:    BCLK->GPIO4, LRC->GPIO5, DIN->GPIO6, VIN 5V
 *                 (clocks shared with the mic: S3 is I2S clock master)
 *   WS2812:       GPIO48, GRB order, 1 LED
 *   BOOT button:  GPIO0, active low. Press = start/stop recording (toggle).
 *
 * Boot: right after reset, before WiFi, play 0.5 s of 440 Hz on the speaker
 * so a working speaker path is audible even if the network never comes up.
 * I2S bus is 48 kHz stereo s32 in both directions (the layout that
 * already beeps on this amp). Recording decimates the right slot to
 * 16 kHz mono s16 for the WAV.
 * Loop: idle listens for the wake word (ESP-SR WakeNet, built-in
 * wn9_xiaojianxiaojian_tts2 "小箭小箭"; BOOT press is the manual
 * fallback) -> on wake, play "我在" then record (BOOT rounds skip
 * the acknowledgement) (max 60s, 16kHz mono s16, stops on 1.5 s of
 * silence) -> play "收到" -> POST /voice -> download-then-play the
 * reply segments (48kHz stereo s32, v18 lesson: never stream-play a
 * reply that is still downloading) -> idle. A round with no detected
 * speech is discarded WITHOUT upload, so silence never reaches the
 * server or spends ASR quota.
 * LED: idle green, recording blue, waiting yellow, playing magenta,
 * error red. WiFi not connected: red until connected.
 *
 * Config (menuconfig -> Robot v1 config): WiFi SSID/pass, backend URL,
 * device token. The DashScope API key is NEVER in firmware -- it lives
 * only on the ECS backend.
 */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ack_shoudao.h"
#include "ack_wozai.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "esp_random.h"
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

static const char *TAG = "robot_v1";

#define MIC_SCK_GPIO   4
#define MIC_WS_GPIO    5
#define MIC_SD_GPIO    7
#define SPK_DIN_GPIO   6
#define LED_GPIO       48
#define BOOT_GPIO      0

#define SPK_SAMPLE_RATE 48000
#define REC_SAMPLE_RATE 16000
#define REC_MAX_SEC     60
#define REC_BUF_BYTES   (REC_SAMPLE_RATE * 2 * REC_MAX_SEC)   /* 1.9 MB */
#define PLAY_BUF_BYTES  (4 * 1024 * 1024)                     /* ~10 s at 48k s32 stereo */

/* Segment-fetch escape (user decision 2026-10-27: purple must be
   gone within 40 s of silence, patience breaks beyond that). One
   segment is ~300 KB: ~6 s down at 55 KB/s plus ~5 s Kokoro synth,
   so these leave 2x margin and the whole escape fits in 40 s:
   HTTP timeout 30 s, prefetch wait cap 35 s from the end of the
   segment being played, sync retry only within 5 s of a fast
   failure (5 + 30 = 35 s), then red 1 s -> green. */
#define SEG_HTTP_TIMEOUT_MS 30000
#define SEG_FETCH_WAIT_MS   35000
#define SEG_RETRY_WINDOW_MS 5000

/* Wake word + silence handling (user decisions 2026-10-30/31):
   - idle listens with ESP-SR WakeNet (built-in
     wn9_xiaojianxiaojian_tts2, "小箭小箭", replacing "你好小鑫"
     and "Hi,乐鑫" which were the earlier words);
     BOOT stays as manual fallback.
   - recording stops on 1.5 s of silence (15 x 100 ms windows), or on
     a 3 s window-average below the silence threshold when spikes
     keep resetting the 1.5 s counter, or the old BOOT toggle /
     60 s cap; a round with no detected speech at all (5 s after
     the 500 ms echo grace) is discarded WITHOUT upload, so silence
     never reaches the server or burns ASR quota.
   - energy thresholds are a starting point, tuned from the serial
     peak= log: room noise on this mic is typically far below 400,
     near-mouth speech averages well above 800 per 100 ms window. */
#define VAD_WINDOW_SAMPLES 1600   /* 100 ms at 16 kHz */
#define VAD_SPEECH_AVG     800    /* floor: never lower than this */
#define VAD_SILENCE_AVG    400    /* floor: never lower than this */
#define VAD_SILENCE_WINDOWS 15    /* 1.5 s */
#define VAD_NO_SPEECH_WINDOWS 50  /* 5 s */
/* Echo grace (bug fix 2026-10-31): the first 500 ms of a round are
   recorded but do not arm speech detection. The just-played "我在"
   is loud next to the mic, and its tail alone used to arm speech;
   after that, ordinary room noise (>= silence threshold) kept
   resetting the silence counter, so a silent round ran to the 60 s
   cap with the LED stuck blue. Thresholds below are floors: the
   real thresholds scale with the ambient level tracked during idle
   listening (see g_noise_avg), so a noisy room still counts as
   silence instead of hanging the round. */
#define VAD_GRACE_WINDOWS 5       /* 500 ms */
/* Ambient level tracker, updated only while idle-listening (never
   from the wake word itself: chunks louder than 2x the estimate are
   ignored, so speech and the wake word cannot inflate it). */
static int g_noise_avg = 0;

static i2s_chan_handle_t mic_chan, spk_chan;
static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_encoder;
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

/* ---------- WS2812 via RMT bytes encoder (GRB, MSB first) ---------- */
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t cfg = {.loop_count = 0};
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, grb, sizeof(grb), &cfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

static void led_init(void)
{
    rmt_tx_channel_config_t ch = {
        .gpio_num = LED_GPIO, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&ch, &led_chan));
    /* 10 MHz ticks: T0 = 350ns/800ns, T1 = 700ns/600ns, reset handled by gap */
    rmt_bytes_encoder_config_t enc = {
        .bit0 = {.duration0 = 4, .level0 = 1, .duration1 = 8, .level1 = 0},
        .bit1 = {.duration0 = 7, .level0 = 1, .duration1 = 6, .level1 = 0},
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
    led_set(255, 0, 0); /* red until WiFi is up */
}

/* ---------- WiFi ---------- */
static void wifi_handler(void *a, esp_event_base_t b, int32_t id, void *d)
{
    if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (b == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
}

static void wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_ROBOT_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_ROBOT_WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to WiFi %s ...", CONFIG_ROBOT_WIFI_SSID);
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
    led_set(0, 255, 0);
}

/* ---------- I2S mic (RX) + speaker (TX), one shared bus ---------- */
static void i2s_init(void)
{
    /* Same wiring as the sketch that already beeps on this board:
       one STD bus, 48 kHz, 32-bit stereo, dout and din both set.
       ESP-IDF shares BCLK/WS only when the two sides match. A 16 kHz
       mono mic plus a 48 kHz stereo speaker leaves the pins on a clock
       that is not running, and the amp stays silent.
       TX is initialized first so it stays the clock master. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* underrun plays silence, not a stuck sample */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &spk_chan, &mic_chan));

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK_GPIO, .ws = MIC_WS_GPIO,
                     .dout = SPK_DIN_GPIO, .din = MIC_SD_GPIO,
                     .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
    };
    /* Explicit width so the driver's duplex compare does not treat
       SLOT_BIT_WIDTH_AUTO as different from 32. */
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(spk_chan, &cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_chan, &cfg));

    i2s_chan_info_t info = {0};
    ESP_ERROR_CHECK(i2s_channel_get_info(spk_chan, &info));
    ESP_LOGI(TAG, "I2S duplex 48k stereo s32, shared clock %s",
             info.pair_chan ? "on" : "OFF");
}

/* ---------- Record (INMP441 32-bit -> s16), upload, play ---------- */
static int16_t *rec_buf;
static uint8_t *play_buf;

/* Enable both I2S directions with silence preloaded: the speaker
   channel is the clock master, so the mic slave sees no BCLK until
   the speaker side is enabled, and preloading zeros keeps enable
   from blasting a leftover sample out the speaker into the mic. */
static void i2s_enable_for_mic(void)
{
    static int32_t zeros[240 * 2];
    memset(zeros, 0, sizeof(zeros));
    size_t loaded = 0;
    while (i2s_channel_preload_data(spk_chan, zeros, sizeof(zeros), &loaded) == ESP_OK &&
           loaded == sizeof(zeros)) {
        loaded = 0;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(spk_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_chan));
}

static size_t record_audio(bool *had_speech)
{
    /* Bus is 48 kHz stereo. INMP441 (L/R high) sits in the right slot.
       Keep 3 samples and store the average so the WAV stays 16 kHz. */
    i2s_enable_for_mic();
    static int32_t raw[240 * 2];
    size_t nbytes = 0;
    int acc = 0;
    int acc_n = 0;
    /* Energy VAD on 100 ms windows of the decimated 16 kHz stream.
       Thresholds scale with the ambient level measured while idle:
       speech needs 4x ambient (floor 800), silence is 2x ambient
       (floor 400), so room noise counts as silence instead of
       keeping the round alive. Speech threshold is capped so a
       very noisy room does not demand shouting. */
    int speech_thresh = g_noise_avg * 4;
    if (speech_thresh < VAD_SPEECH_AVG) speech_thresh = VAD_SPEECH_AVG;
    if (speech_thresh > 4000) speech_thresh = 4000;
    int silence_thresh = g_noise_avg * 2;
    if (silence_thresh < VAD_SILENCE_AVG) silence_thresh = VAD_SILENCE_AVG;
    ESP_LOGI(TAG, "vad noise=%d speech_thr=%d silence_thr=%d",
             g_noise_avg, speech_thresh, silence_thresh);
    int32_t win_sum = 0;
    int win_n = 0;
    int win_count = 0;
    int silence_windows = 0;
    bool speech = false;
    /* Slow silence path (fix 2026-10-31): the last 30 window averages
       (3 s). The fast path needs 15 CONSECUTIVE quiet windows, but
       near-threshold noise -- breathing, a fan, room sounds -- spikes
       above the line often enough to reset that counter forever
       (the 30-40 s hangs the user measured). If the 3 s mean is
       below the silence threshold, the user has stopped even if a
       few windows spiked, so stop. Genuine soft speech keeps the
       mean above the line and is never cut by this path. */
    int win_hist[30];
    int hist_n = 0;
    int hist_idx = 0;
    const char *stop_why = "cap";
    while (nbytes < REC_BUF_BYTES) {
        size_t got = 0;
        if (i2s_channel_read(mic_chan, raw, sizeof(raw), &got, 1000 / portTICK_PERIOD_MS) != ESP_OK) {
            stop_why = "i2s-error";
            break;
        }
        size_t frames = got / 8;
        bool stop = false;
        for (size_t i = 0; i < frames && nbytes + 2 <= REC_BUF_BYTES; i++) {
            acc += (int)(raw[i * 2 + 1] >> 16);
            if (++acc_n == 3) {
                int16_t s = (int16_t)(acc / 3);
                rec_buf[nbytes / 2] = s;
                nbytes += 2;
                acc = 0;
                acc_n = 0;
                win_sum += s < 0 ? -(int32_t)s : s;
                if (++win_n == VAD_WINDOW_SAMPLES) {
                    int avg = (int)(win_sum / VAD_WINDOW_SAMPLES);
                    win_sum = 0;
                    win_n = 0;
                    win_count++;
                    if (win_count <= VAD_GRACE_WINDOWS) {
                        /* Echo tail: audio kept, decision skipped. */
                    } else if (!speech) {
                        if (avg >= speech_thresh) {
                            speech = true;
                        } else if (win_count >= VAD_GRACE_WINDOWS + VAD_NO_SPEECH_WINDOWS) {
                            stop = true;
                            stop_why = "no-speech";
                            break;
                        }
                    } else {
                        win_hist[hist_idx] = avg;
                        hist_idx = (hist_idx + 1) % 30;
                        if (hist_n < 30) hist_n++;
                        if (avg < silence_thresh) {
                            if (++silence_windows >= VAD_SILENCE_WINDOWS) {
                                stop = true;
                                stop_why = "silence";
                                break;
                            }
                        } else {
                            silence_windows = 0;
                        }
                        if (hist_n == 30) {
                            int32_t hsum = 0;
                            for (int h = 0; h < 30; h++) hsum += win_hist[h];
                            if (hsum / 30 < silence_thresh) {
                                stop = true;
                                stop_why = "silence-avg";
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (stop) break;
        /* stop when BOOT is pressed again (toggle) */
        if (gpio_get_level(BOOT_GPIO) == 0) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (gpio_get_level(BOOT_GPIO) == 0) {
                stop_why = "boot";
                break;
            }
        }
    }
    ESP_LOGI(TAG, "record stop: %s", stop_why);
    ESP_ERROR_CHECK(i2s_channel_disable(mic_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(spk_chan));
    *had_speech = speech;
    return nbytes;
}

/* ---------- Wake word: ESP-SR AFE + WakeNet (wn9_xiaojianxiaojian_tts2) ---------- */
/* User 2026-10-31: the built-in "小箭小箭" model, replacing "你好小鑫"
   and "Hi,乐鑫"; a custom word later means swapping the selected
   model, not changing this code. AFE runs on the same decimated
   16 kHz mono
   stream the recorder uses. If AFE init fails, the board falls back
   to BOOT-only (the old behaviour) instead of bricking voice input. */
static const esp_afe_sr_iface_t *afe_handle;
static esp_afe_sr_data_t *afe_data;
static bool g_afe_ok = false;

static void afe_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed: model partition missing?");
        return;
    }
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }
    afe_handle = esp_afe_handle_from_config(cfg);
    if (!afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg);
        return;
    }
    afe_data = afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!afe_data) {
        ESP_LOGE(TAG, "AFE create failed");
        return;
    }
    g_afe_ok = true;
    ESP_LOGI(TAG, "wake word ready: say 小箭小箭 (wn9_xiaojianxiaojian_tts2)");
}

typedef enum { LISTEN_NONE, LISTEN_WAKE, LISTEN_BOOT } listen_result_t;

/* Idle listening: mic on, speaker clock running on silence, AFE
   fed with the decimated 16 kHz mono stream. Returns on wake-word
   detection or on a debounced BOOT press (manual fallback). */
static listen_result_t listen_for_wake(void)
{
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);
    if (feed_nch != 1) {
        ESP_LOGE(TAG, "unexpected AFE feed channels: %d", feed_nch);
        return LISTEN_NONE;
    }
    int16_t *feed_buf = malloc((size_t)feed_chunksize * sizeof(int16_t));
    if (!feed_buf) return LISTEN_NONE;
    i2s_enable_for_mic();
    static int32_t raw[240 * 2];
    int acc = 0;
    int acc_n = 0;
    size_t feed_n = 0;
    listen_result_t res = LISTEN_NONE;
    while (res == LISTEN_NONE) {
        size_t got = 0;
        if (i2s_channel_read(mic_chan, raw, sizeof(raw), &got, 1000 / portTICK_PERIOD_MS) != ESP_OK)
            continue;
        size_t frames = got / 8;
        for (size_t i = 0; i < frames; i++) {
            acc += (int)(raw[i * 2 + 1] >> 16);
            if (++acc_n == 3) {
                feed_buf[feed_n++] = (int16_t)(acc / 3);
                acc = 0;
                acc_n = 0;
                if (feed_n >= (size_t)feed_chunksize) {
                    /* Track the ambient level for the recorder's
                       adaptive thresholds. Chunks louder than 2x
                       the estimate are ignored, so the wake word
                       itself and speech cannot inflate it. */
                    int32_t csum = 0;
                    for (size_t k = 0; k < feed_n; k++) {
                        int32_t s = feed_buf[k];
                        csum += s < 0 ? -s : s;
                    }
                    int cavg = (int)(csum / (int32_t)feed_n);
                    if (g_noise_avg == 0 || cavg < g_noise_avg * 2) {
                        g_noise_avg = g_noise_avg == 0
                            ? cavg : (g_noise_avg * 7 + cavg) / 8;
                    }
                    afe_handle->feed(afe_data, feed_buf);
                    feed_n = 0;
                    afe_fetch_result_t *r = afe_handle->fetch(afe_data);
                    if (r && r->wakeup_state == WAKENET_DETECTED) {
                        ESP_LOGI(TAG, "wake word detected");
                        res = LISTEN_WAKE;
                        break;
                    }
                }
            }
        }
        if (res != LISTEN_NONE) break;
        /* Manual fallback: BOOT press starts a round immediately. */
        if (gpio_get_level(BOOT_GPIO) == 0) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (gpio_get_level(BOOT_GPIO) == 0) {
                while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(10 / portTICK_PERIOD_MS);
                res = LISTEN_BOOT;
            }
        }
    }
    ESP_ERROR_CHECK(i2s_channel_disable(mic_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(spk_chan));
    free(feed_buf);
    return res;
}

/* NOTE (bug fix 2026-10-26): esp_http_client_get_header() reads
   REQUEST headers, not response headers — response headers need
   get_response_header() (behind a Kconfig) or the ON_HEADER event.
   We parse response headers from the event, no Kconfig needed. */
static char g_audio_format[24];
static bool g_seg_mode;
static int g_seg_total = 1;
static char g_session[40];
/* Download target for http_event: set before each request so the
   prefetch task can download into a different buffer than the one
   being played. */
static uint8_t *g_dl_buf;
static size_t g_dl_cap;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    static size_t play_len;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "X-Audio-Format") == 0) {
            snprintf(g_audio_format, sizeof(g_audio_format), "%s", e->header_value);
        } else if (strcasecmp(e->header_key, "X-Segments-Total") == 0) {
            g_seg_total = atoi(e->header_value);
            if (g_seg_total < 1) g_seg_total = 1;
        } else if (strcasecmp(e->header_key, "X-Session-Id") == 0) {
            snprintf(g_session, sizeof(g_session), "%s", e->header_value);
        }
    }
    if (e->event_id == HTTP_EVENT_ON_DATA && g_dl_buf &&
        play_len + e->data_len <= g_dl_cap) {
        memcpy(g_dl_buf + play_len, e->data, e->data_len);
        play_len += e->data_len;
    }
    if (e->event_id == HTTP_EVENT_ON_FINISH) {
        extern size_t g_play_len;
        g_play_len = play_len;
        play_len = 0;
    }
    return ESP_OK;
}
size_t g_play_len;

/* Segmented replies (backend segments protocol, 2026-10-25): the
   backend splits answers into <=45-char segments, downsamples each
   to 16 kHz mono s16, and returns segment 0 on POST /voice plus
   X-Segments-Total / X-Session-Id headers. Later segments come
   from GET /voice_segment. g_seg_mode is false only when the
   backend did not send the new headers (old single-blob format).
   (Globals live above http_event, which parses the headers.) */

static bool upload_and_download(const int16_t *pcm, size_t nbytes)
{
    /* Build a WAV header in front of the PCM for the backend. */
    size_t wav_len = 44 + nbytes;
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return false;
    /* Standard 44-byte WAV header. The old code wrote the fmt length at
       +12, overwriting the "fmt " chunk ID and shifting every field 4
       bytes early — strict decoders (DashScope, Python wave) rejected
       the file with DECODE_ERROR. */
    memcpy(wav, "RIFF", 4);
    uint32_t v;
    v = 36 + nbytes; memcpy(wav + 4, &v, 4);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    v = 16; memcpy(wav + 16, &v, 4);
    uint16_t s16;
    s16 = 1; memcpy(wav + 20, &s16, 2);
    s16 = 1; memcpy(wav + 22, &s16, 2);
    v = REC_SAMPLE_RATE; memcpy(wav + 24, &v, 4);
    v = REC_SAMPLE_RATE * 2; memcpy(wav + 28, &v, 4);
    s16 = 2; memcpy(wav + 32, &s16, 2);
    s16 = 16; memcpy(wav + 34, &s16, 2);
    memcpy(wav + 36, "data", 4);
    v = nbytes; memcpy(wav + 40, &v, 4);
    memcpy(wav + 44, pcm, nbytes);

    g_play_len = 0;
    g_seg_mode = false;
    g_seg_total = 1;
    g_session[0] = '\0';
    g_audio_format[0] = '\0';
    g_dl_buf = play_buf;
    g_dl_cap = PLAY_BUF_BYTES;
    esp_http_client_config_t cfg = {
        .url = CONFIG_ROBOT_BACKEND_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .timeout_ms = 180000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_ROBOT_DEVICE_TOKEN);
    esp_http_client_set_post_field(client, (const char *)wav, wav_len);
    esp_err_t err = esp_http_client_perform(client);
    long status = esp_http_client_get_status_code(client);
    /* Segment headers were parsed by http_event during perform. */
    if (err == ESP_OK && status == 200 &&
        strcmp(g_audio_format, "s16le-16k-mono") == 0) {
        g_seg_mode = true;
    }
    esp_http_client_cleanup(client);
    heap_caps_free(wav);
    if (err != ESP_OK || status != 200 || g_play_len == 0 ||
        (g_seg_mode ? (g_play_len % 2 != 0) : (g_play_len % 24 != 0))) {
        ESP_LOGE(TAG, "voice call failed err=%d status=%ld play_len=%zu seg=%d",
                 err, status, g_play_len, g_seg_total);
        return false;
    }
    ESP_LOGI(TAG, "reply segment 0/%d %zu bytes", g_seg_total, g_play_len);
    return true;
}

/* Fetch one later segment into the given buffer. */
static bool fetch_segment_to(int index, uint8_t *buf, size_t cap)
{
    /* Derive the segment URL from the /voice URL. */
    char url[256];
    const char *base = CONFIG_ROBOT_BACKEND_URL;
    size_t blen = strlen(base);
    if (blen > 6 && strcmp(base + blen - 6, "/voice") == 0) blen -= 6;
    if (blen >= sizeof(url) - 64) return false;
    memcpy(url, base, blen);
    url[blen] = '\0';
    snprintf(url + blen, sizeof(url) - blen,
             "/voice_segment?session=%s&index=%d", g_session, index);
    g_dl_buf = buf;
    g_dl_cap = cap;
    g_play_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event,
        .timeout_ms = SEG_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_ROBOT_DEVICE_TOKEN);
    esp_err_t err = esp_http_client_perform(client);
    long status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || g_play_len == 0 || g_play_len % 2 != 0) {
        ESP_LOGE(TAG, "segment %d failed err=%d status=%ld play_len=%zu",
                 index, err, status, g_play_len);
        return false;
    }
    ESP_LOGI(TAG, "reply segment %d/%d %zu bytes", index + 1, g_seg_total, g_play_len);
    return true;
}

/* Prefetch task (user decision 2026-10-26): while the main task
   plays segment N, this task downloads segment N+1 into the other
   buffer, so the 1-1.5 s download gap between segments goes away.
   It never touches I2S, only HTTP. The recording buffer is free
   by playback time (the upload already finished), so it doubles
   as the second segment buffer — no extra allocation. */
static volatile int g_fetch_index = -1;
static volatile bool g_fetch_done = true;
static volatile bool g_fetch_ok = false;
static volatile bool g_fetch_exit = false;
static volatile size_t g_fetch_len = 0;
static uint8_t *g_fetch_buf = NULL;
static size_t g_fetch_cap = 0;
static TaskHandle_t g_fetch_task = NULL;

/* Generation guard: stop_fetch_task only flags exit and waits
   200 ms, so a task stuck in a (<=30 s) HTTP fetch may still be
   alive when the next round starts. A stale task must never touch
   the new round's fetch state or buffers, so each task carries the
   generation it was created with and drops its result -- and the
   shared flags -- once a newer generation exists. */
static volatile int g_fetch_gen = 0;

static void fetch_task(void *arg)
{
    int my_gen = (int)(intptr_t)arg;
    while (!g_fetch_exit && my_gen == g_fetch_gen) {
        if (g_fetch_index >= 0) {
            int idx = g_fetch_index;
            uint8_t *buf = g_fetch_buf;
            size_t cap = g_fetch_cap;
            bool okf = fetch_segment_to(idx, buf, cap);
            if (!g_fetch_exit && my_gen == g_fetch_gen) {
                g_fetch_ok = okf;
                g_fetch_len = g_play_len;
                g_fetch_index = -1;
                g_fetch_done = true;
            }
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    vTaskDelete(NULL);
}

static void request_fetch(int index, uint8_t *buf, size_t cap)
{
    g_fetch_buf = buf;
    g_fetch_cap = cap;
    g_fetch_done = false;
    g_fetch_ok = false;
    g_fetch_len = 0;
    g_fetch_index = index;
}

static void stop_fetch_task(void)
{
    if (g_fetch_task) {
        g_fetch_exit = true;
        vTaskDelay(200 / portTICK_PERIOD_MS);
        g_fetch_task = NULL;
    }
}

/* Play a buffer as 16 kHz mono s16, upsampled x3 to the 48 kHz
   stereo s32 bus (same layout as the ack phrases). */
static void play_segment_audio_buf(const uint8_t *buf, size_t len_bytes)
{
    ESP_ERROR_CHECK(i2s_channel_enable(spk_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_chan));
    static int32_t out[256 * 3 * 2];
    const int16_t *samples = (const int16_t *)buf;
    size_t total = len_bytes / 2;
    for (size_t i = 0; i < total;) {
        size_t n = total - i;
        if (n > 256) n = 256;
        for (size_t j = 0; j < n; j++) {
            int32_t s = (int32_t)samples[i + j] << 16;
            for (int k = 0; k < 3; k++) {
                out[(j * 3 + k) * 2] = s;
                out[(j * 3 + k) * 2 + 1] = s;
            }
        }
        size_t bytes = n * 3 * 8, off = 0;
        while (off < bytes) {
            size_t w = 0;
            if (i2s_channel_write(spk_chan, (uint8_t *)out + off, bytes - off, &w,
                                  1000 / portTICK_PERIOD_MS) != ESP_OK ||
                w == 0) {
                break;
            }
            off += w;
        }
        i += n;
    }
    ESP_ERROR_CHECK(i2s_channel_disable(mic_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(spk_chan));
}

/* Play segment 0 (already in play_buf), prefetching each next
   segment into the other buffer while the current one plays. */
static bool play_segment_loop(void)
{
    uint8_t *cur = play_buf;
    size_t cur_len = g_play_len;
    uint8_t *nxt = (uint8_t *)rec_buf;
    bool ok = true;
    g_fetch_exit = false;
    g_fetch_index = -1;
    g_fetch_done = true;
    if (g_seg_total > 1) {
        /* Task creation can fail (out of memory): then no prefetch
           is requested and the sync retry below covers segment 1. */
        g_fetch_gen++;
        if (xTaskCreate(fetch_task, "seg_fetch", 8192,
                        (void *)(intptr_t)g_fetch_gen, 5,
                        &g_fetch_task) == pdPASS) {
            request_fetch(1, nxt, REC_BUF_BYTES);
        }
    }
    for (int i = 0; i < g_seg_total; i++) {
        play_segment_audio_buf(cur, cur_len);
        if (i + 1 >= g_seg_total) break;
        /* Bounded wait (user decision 2026-10-27, 40 s silence
           budget): playback (~7-9 s) normally outlasts the prefetch,
           but if the fetch is still running when this segment ends,
           wait at most SEG_FETCH_WAIT_MS, then abandon the stream.
           Never retry into the same buffer while the task may still
           run -- that race is why the old code waited forever and
           purple never returned to green. On timeout the caller
           takes the existing red 1 s -> green failure path. */
        TickType_t wait_start = xTaskGetTickCount();
        while (!g_fetch_done &&
               (xTaskGetTickCount() - wait_start) * portTICK_PERIOD_MS <
                   SEG_FETCH_WAIT_MS) {
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
        if (!g_fetch_done) {
            ESP_LOGE(TAG, "segment %d fetch timeout after %d ms, "
                     "abandoning stream", i + 1, SEG_FETCH_WAIT_MS);
            ok = false;
            break;
        }
        if (!g_fetch_ok) {
            /* One sync retry, only if this fast failure still leaves
               room inside the 40 s budget (5 s + 30 s HTTP timeout).
               A failure found later in the wait means abandon. */
            TickType_t elapsed =
                (xTaskGetTickCount() - wait_start) * portTICK_PERIOD_MS;
            if (elapsed >= SEG_RETRY_WINDOW_MS ||
                !fetch_segment_to(i + 1, nxt, REC_BUF_BYTES)) {
                ok = false;
                break;
            }
            g_fetch_len = g_play_len;
        }
        uint8_t *tmp = cur;
        cur = nxt;
        nxt = tmp;
        cur_len = g_fetch_len;
        if (i + 2 < g_seg_total) request_fetch(i + 2, nxt, REC_BUF_BYTES);
    }
    stop_fetch_task();
    return ok;
}

/* 0.5 s A4, same sample layout as the sketch that already beeps here:
   16-bit amplitude in the high half of each 32-bit slot, both channels.
   Both directions are enabled because the shared BCLK/WS only runs while
   the master (and, if the pins landed on the other side, the slave) is on. */
static void play_startup_tone(void)
{
    static int32_t buf[128 * 2];
    const int frames = SPK_SAMPLE_RATE / 2;
    const int flush = 6 * 240;
    const float step = 6.283185307f * 440.0f / (float)SPK_SAMPLE_RATE;

    ESP_LOGI(TAG, "speaker check: 440 Hz for 500 ms");
    ESP_ERROR_CHECK(i2s_channel_enable(spk_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_chan));
    int sent = 0;
    const int total = frames + flush;
    while (sent < total) {
        int n = total - sent;
        if (n > 128) n = 128;
        for (int i = 0; i < n; i++) {
            int32_t s = 0;
            if (sent + i < frames) {
                s = (int32_t)(12000.0f * sinf(step * (float)(sent + i))) << 16;
            }
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
        }
        size_t bytes = (size_t)n * 8;
        size_t off = 0;
        while (off < bytes) {
            size_t w = 0;
            if (i2s_channel_write(spk_chan, (uint8_t *)buf + off, bytes - off, &w,
                                  1000 / portTICK_PERIOD_MS) != ESP_OK || w == 0) {
                ESP_LOGE(TAG, "startup tone write failed");
                goto done;
            }
            off += w;
        }
        sent += n;
    }
done:
    ESP_ERROR_CHECK(i2s_channel_disable(mic_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(spk_chan));
}

/* Waiting prompts REMOVED (user 2026-10-31): after "收到", no more
   feedback voices -- the yellow phase stays silent until the reply
   itself plays. The phrase headers stay in the repo unused, so the
   feature can come back with one task again. */

static void play_audio(void)
{
    ESP_ERROR_CHECK(i2s_channel_enable(spk_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_chan));
    size_t written = 0;
    while (written < g_play_len) {
        size_t w = 0;
        i2s_channel_write(spk_chan, play_buf + written, g_play_len - written, &w,
                          1000 / portTICK_PERIOD_MS);
        if (w == 0) break;
        written += w;
    }
    ESP_ERROR_CHECK(i2s_channel_disable(mic_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(spk_chan));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_config_t btn = {.pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&btn));
    led_init();
    i2s_init();
    play_startup_tone();
    wifi_init();
    rec_buf = heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    play_buf = heap_caps_malloc(PLAY_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rec_buf || !play_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        led_set(255, 0, 0);
        return;
    }
    afe_init();
    ESP_LOGI(TAG, "robot v1 ready: say 小箭小箭 or press BOOT to talk");
    while (1) {
        listen_result_t trig = LISTEN_NONE;
        if (g_afe_ok) {
            trig = listen_for_wake();
        } else {
            /* AFE unavailable: BOOT-only fallback, as before. */
            if (gpio_get_level(BOOT_GPIO) == 0) {
                vTaskDelay(50 / portTICK_PERIOD_MS);
                if (gpio_get_level(BOOT_GPIO) == 0) {
                    while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(10 / portTICK_PERIOD_MS);
                    trig = LISTEN_BOOT;
                }
            }
        }
        if (trig == LISTEN_NONE) {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }
        led_set(0, 0, 255); /* recording */
        if (trig == LISTEN_WAKE) {
            /* Wake acknowledgement (user 2026-10-31): say "我在" so
               the user knows the board heard the wake word and can
               start speaking now. BOOT rounds skip it: the press
               itself is the confirmation. */
            play_segment_audio_buf((const uint8_t *)ACK_WOZAI_SAMPLES,
                                   ACK_WOZAI_COUNT * 2);
        }
        bool had_speech = false;
        size_t n = record_audio(&had_speech);
        int32_t peak = 0;
        for (size_t i = 0; i < n / 2; i++) {
            int32_t s = rec_buf[i] < 0 ? -(int32_t)rec_buf[i] : rec_buf[i];
            if (s > peak) peak = s;
        }
        ESP_LOGI(TAG, "recorded %zu bytes peak=%ld first=%d speech=%d",
                 n, (long)peak, n >= 2 ? rec_buf[0] : 0, had_speech);
        if (!had_speech || n <= 16000) {
            /* Silence (or a too-short blip) never reaches the server:
               no upload, no ASR quota spent, straight back to idle. */
            ESP_LOGI(TAG, "no speech in this round, discarding");
            led_set(0, 255, 0); /* idle */
            continue;
        }
        /* Acknowledgement, then the existing upload/wait/play flow. */
        play_segment_audio_buf((const uint8_t *)ACK_SHOUDAO_SAMPLES,
                               ACK_SHOUDAO_COUNT * 2);
            if (n > 16000) {
                led_set(255, 255, 0); /* waiting */
                /* No feedback voices after "收到" (user 2026-10-31):
                   the yellow phase stays silent until the reply
                   itself plays. */
                bool ok = upload_and_download(rec_buf, n);
                if (ok) {
                    led_set(255, 0, 255); /* playing */
                    if (g_seg_mode) ok = play_segment_loop();
                    else play_audio();
                }
                if (!ok) {
                    led_set(255, 0, 0);
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                }
            }
        led_set(0, 255, 0); /* idle */
    }
}
