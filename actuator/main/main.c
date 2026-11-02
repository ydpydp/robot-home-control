/* RHC actuator (N8R8) bench skeleton: BLE peripheral + LED as relay stand-in.
 *
 * Board: ESP32-S3 N8R8. The bench LED stands in for the relay; the relay
 * control is active-low (see below). Boot state is OFF (relay-safe).
 * Relay control pin: GPIO12, ACTIVE-LOW: LOW = ON (closed), HIGH = OFF (open).
 * Boot default is HIGH (OFF, relay-safe). GPIO48 is driven HIGH at boot
 * as a power indicator.
 * GPIO19 is NOT used: it is native USB D-, and N8R8 flashes over native USB,
 * so a load there can break enumeration.
 * Avoided pins: strapping 0/3/45/46, USB 19/20, WS2812 47, UART0 43/44.
 * Commands are plain text and idempotent; unknown commands are ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "RHC_ACT";

/* Relay control pin (bench LED wired here). Active-low: LOW = ON, HIGH = OFF. */
#define ACTUATOR_CTRL_GPIO 12
/* Power indicator: driven HIGH at boot. */
#define ACTUATOR_PWR_GPIO 48

/* BLE identifiers (also in docs/语音控家电v1计划.md §3.2). */
#define RHC_DEVICE_NAME "RHC-Actuator"

/* 128-bit UUIDs, little-endian byte order (reverse of the string form):
 * service c71566d8-7307-4f3e-a82f-bb0be617b22d
 * command 8e2e563b-410a-4ce4-8a16-c1a43b3d5f13
 * state   fe07c9e6-ece5-4cc7-8467-531233dc6565
 */
static const ble_uuid128_t rhc_svc_uuid =
    BLE_UUID128_INIT(0x2d, 0xb2, 0x17, 0xe6, 0x0b, 0xbb, 0x2f, 0xa8,
                    0x3e, 0x4f, 0x07, 0x73, 0xd8, 0x66, 0x15, 0xc7);
static const ble_uuid128_t rhc_cmd_uuid =
    BLE_UUID128_INIT(0x13, 0x5f, 0x3d, 0x3b, 0xa4, 0xc1, 0x16, 0x8a,
                    0xe4, 0x4c, 0x0a, 0x41, 0x3b, 0x56, 0x2e, 0x8e);
static const ble_uuid128_t rhc_state_uuid =
    BLE_UUID128_INIT(0x65, 0x65, 0xdc, 0x33, 0x12, 0x53, 0x67, 0x84,
                    0xc7, 0x4c, 0xe5, 0xec, 0xe6, 0xc9, 0x07, 0xfe);

static uint16_t rhc_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t rhc_state_val_handle = 0;
static int rhc_state_on = 0; /* 0 = OFF (LED dark), 1 = ON (LED lit) */

static void rhc_apply_state(int on)
{
    rhc_state_on = on ? 1 : 0;
    gpio_set_level(ACTUATOR_CTRL_GPIO, rhc_state_on ? 0 : 1);
    ESP_LOGI(TAG, "state -> %s", rhc_state_on ? "ON" : "OFF");
}

static void rhc_notify_state(void)
{
    if (rhc_conn_handle == BLE_HS_CONN_HANDLE_NONE || rhc_state_val_handle == 0) {
        return;
    }
    const char *s = rhc_state_on ? "ON" : "OFF";
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (om == NULL) {
        ESP_LOGE(TAG, "notify mbuf alloc failed");
        return;
    }
    int rc = ble_gatts_notify_custom(rhc_conn_handle, rhc_state_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "notify failed rc=%d", rc);
    }
}

/* Parse "ON" / "OFF" / "BRI:<n>"; anything else is ignored (no guessing). */
static void rhc_handle_command(const uint8_t *data, uint16_t len)
{
    char buf[32];
    if (len >= sizeof(buf)) {
        ESP_LOGW(TAG, "command too long (%u), ignored", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }
    ESP_LOGI(TAG, "command: '%s'", buf);
    if (strcmp(buf, "ON") == 0) {
        rhc_apply_state(1);
    } else if (strcmp(buf, "OFF") == 0) {
        rhc_apply_state(0);
    } else if (strncmp(buf, "BRI:", 4) == 0) {
        /* No brightness on a relay: any level > 0 means ON (pin low). */
        rhc_apply_state(atoi(buf + 4) > 0);
    } else {
        ESP_LOGW(TAG, "unknown command, ignored");
        return;
    }
    rhc_notify_state(); /* Receipt for every accepted command, even repeats. */
}

static int rhc_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ble_uuid_cmp(ctxt->chr->uuid, &rhc_cmd_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t tmp[32];
            if (len > sizeof(tmp)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            os_mbuf_copydata(ctxt->om, 0, len, tmp);
            rhc_handle_command(tmp, len);
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &rhc_state_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *s = rhc_state_on ? "ON" : "OFF";
            return os_mbuf_append(ctxt->om, s, strlen(s));
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def rhc_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &rhc_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rhc_cmd_uuid.u,
                .flags = BLE_GATT_CHR_F_WRITE,
                .access_cb = rhc_gatt_access_cb,
            },
            {
                .uuid = &rhc_state_uuid.u,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = rhc_gatt_access_cb,
            },
            { 0 },
        },
    },
    { 0 },
};

static void rhc_start_advertising(void);

static int rhc_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            rhc_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected, handle=%u", rhc_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d, re-advertising", event->connect.status);
            rhc_start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected reason=%d, re-advertising", event->disconnect.reason);
        rhc_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        rhc_start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete, restarting");
        rhc_start_advertising();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption changed status=%d", event->enc_change.status);
        break;
    default:
        break;
    }
    return 0;
}

static void rhc_start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    uint8_t own_addr_type;
    int rc;

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t[]){ rhc_svc_uuid };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto failed rc=%d", rc);
        return;
    }
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, rhc_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s", RHC_DEVICE_NAME);
    }
}

static void rhc_on_sync(void)
{
    int rc;
    uint16_t svc_handle = 0;
    char uuid_str[BLE_UUID_STR_LEN];

    rc = ble_gatts_find_svc(&rhc_svc_uuid.u, &svc_handle);
    if (rc == 0) {
        rc = ble_gatts_find_chr(&rhc_svc_uuid.u, &rhc_state_uuid.u, NULL, &rhc_state_val_handle);
        if (rc != 0) {
            ESP_LOGE(TAG, "find state chr failed rc=%d", rc);
        }
    } else {
        ESP_LOGE(TAG, "find svc failed rc=%d", rc);
    }
    ble_uuid_to_str(&rhc_svc_uuid.u, uuid_str);
    ESP_LOGI(TAG, "service UUID: %s", uuid_str);
    rhc_start_advertising();
}

static void rhc_on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset reason=%d", reason);
}

static void rhc_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Relay control (bench LED wired here): boot HIGH = OFF (relay-safe).
     * Power indicator: boot HIGH. */
    gpio_reset_pin(ACTUATOR_CTRL_GPIO);
    gpio_set_direction(ACTUATOR_CTRL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ACTUATOR_CTRL_GPIO, 1);
    gpio_reset_pin(ACTUATOR_PWR_GPIO);
    gpio_set_direction(ACTUATOR_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ACTUATOR_PWR_GPIO, 1);
    rhc_state_on = 0;

    /* Watchdog, fed by the main loop below. IDF already initializes the
     * task watchdog at startup when CONFIG_ESP_TASK_WDT_EN is on (the
     * default), so a second init returns ESP_ERR_INVALID_STATE ("TWDT
     * already initialized") -- that must NOT abort. Tolerate it and just
     * subscribe this task; the effective timeout is then the system
     * default (5 s), which the 1 s feed loop satisfies comfortably.
     * (A second init + ESP_ERROR_CHECK caused the 2026-11-02 boot abort
     * loop on hardware; a VM build cannot catch it, it only fires at
     * runtime.) */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t wdt_ret = esp_task_wdt_init(&wdt_cfg);
    if (wdt_ret != ESP_OK && wdt_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(wdt_ret);
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    ble_hs_cfg.sync_cb = rhc_on_sync;
    ble_hs_cfg.reset_cb = rhc_on_reset;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(RHC_DEVICE_NAME);
    ble_gatts_count_cfg(rhc_gatt_svcs);
    int rc = ble_gatts_add_svcs(rhc_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs failed rc=%d", rc);
        return;
    }
    nimble_port_freertos_init(rhc_host_task);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
