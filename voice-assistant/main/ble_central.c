/* BLE central for the voice assistant -- see ble_central.h.
 *
 * Structure follows the ESP-IDF NimBLE blecent example: one host
 * task running nimble_port_run(), all state changes driven by GAP
 * events, GATT discovery chained service -> characteristics ->
 * CCCD -> subscribe. The execute path (main task) only touches
 * the write + receipt event group, serialized by a mutex.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "ble_central.h"

static const char *TAG = "rhc_ble";

/* Same UUIDs as the actuator (plan §3.2). Parsed from strings so
   the byte order cannot drift from the actuator's table. */
#define RHC_SVC_UUID_STR   "c71566d8-7307-4f3e-a82f-bb0be617b22d"
#define RHC_CMD_UUID_STR   "8e2e563b-410a-4ce4-8a16-c1a43b3d5f13"
#define RHC_STATE_UUID_STR "fe07c9e6-ece5-4cc7-8467-531233dc6565"

#define RHC_READY_BIT   BIT0
#define RHC_RECEIPT_BIT BIT1
#define RHC_RECEIPT_TIMEOUT_MS 2000 /* plan §3.2: receipt within 2 s */

static EventGroupHandle_t rhc_events;
static SemaphoreHandle_t rhc_exec_mutex;
static uint16_t rhc_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t rhc_cmd_handle;
static uint16_t rhc_state_handle;
static uint16_t rhc_svc_start_handle;
static uint16_t rhc_svc_end_handle;
static uint16_t rhc_cccd_handle;
static bool rhc_scanning;
static char rhc_receipt[16]; /* last state-notify payload ("ON"/"OFF") */

static int rhc_gap_event(struct ble_gap_event *event, void *arg);

static void rhc_start_scan(void)
{
    if (rhc_scanning) return;
    static const struct ble_gap_disc_params params = {
        .itvl = 0x0080, .window = 0x0040,
        .filter_policy = 0, .limited = 0, .passive = 1,
        .filter_duplicates = 1,
    };
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no own address type, scan deferred");
        return;
    }
    int rc = ble_gap_disc(own_addr_type, 0, &params, rhc_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        rhc_scanning = true;
    } else {
        ESP_LOGE(TAG, "scan start failed rc=%d", rc);
    }
}

static void rhc_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void rhc_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced, scanning for actuator");
    rhc_start_scan();
}

static void rhc_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- GATT discovery chain --- */

static int rhc_subscribe_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "subscribed to state notify, actuator ready");
        xEventGroupSetBits(rhc_events, RHC_READY_BIT);
    } else {
        ESP_LOGE(TAG, "subscribe failed status=%d, disconnecting",
                 error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int rhc_dsc_disc_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           uint16_t chr_val_handle,
                           const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    (void)chr_val_handle;
    if (error->status == 0 && dsc != NULL) {
        ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0) {
            rhc_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE || rhc_cccd_handle == 0) {
        ESP_LOGE(TAG, "CCCD not found, status=%d, disconnecting",
                 error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    /* Subscribe: write 0x0001 (notifications) to the CCCD. */
    uint8_t val[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, rhc_cccd_handle, val,
                                  sizeof(val), rhc_subscribe_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write failed rc=%d, disconnecting", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int rhc_chr_disc_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        ble_uuid_any_t cmd_uuid, state_uuid;
        ble_uuid_from_str(&cmd_uuid, RHC_CMD_UUID_STR);
        ble_uuid_from_str(&state_uuid, RHC_STATE_UUID_STR);
        if (ble_uuid_cmp(&chr->uuid.u, &cmd_uuid.u) == 0) {
            rhc_cmd_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &state_uuid.u) == 0) {
            rhc_state_handle = chr->val_handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE || rhc_cmd_handle == 0 ||
        rhc_state_handle == 0) {
        ESP_LOGE(TAG, "command/state chars not found, status=%d cmd=%u "
                 "state=%u, disconnecting", error->status,
                 rhc_cmd_handle, rhc_state_handle);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    int rc = ble_gattc_disc_all_dscs(conn_handle, rhc_state_handle,
                                     rhc_svc_end_handle, rhc_dsc_disc_cb,
                                     NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "descriptor discovery failed rc=%d, disconnecting",
                 rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int rhc_svc_disc_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == 0 && svc != NULL) {
        rhc_svc_start_handle = svc->start_handle;
        rhc_svc_end_handle = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || rhc_svc_end_handle == 0) {
        ESP_LOGE(TAG, "RHC service not found, status=%d, disconnecting",
                 error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn_handle, rhc_svc_start_handle,
                                     rhc_svc_end_handle, rhc_chr_disc_cb,
                                     NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "characteristic discovery failed rc=%d, "
                 "disconnecting", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* --- GAP events --- */

static int rhc_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) {
            return 0;
        }
        ble_uuid_any_t svc_uuid;
        bool found = false;
        if (ble_uuid_from_str(&svc_uuid, RHC_SVC_UUID_STR) == 0) {
            for (int i = 0; i < fields.num_uuids128; i++) {
                if (ble_uuid_cmp(&fields.uuids128[i].u, &svc_uuid.u) == 0) {
                    found = true;
                    break;
                }
            }
        }
        ble_hs_adv_parse_free();
        if (!found) {
            return 0;
        }
        /* Actuator found: stop scanning, connect. */
        ble_gap_disc_cancel();
        rhc_scanning = false;
        static const struct ble_gap_conn_params conn_params = {
            .scan_itvl = 0x0010, .scan_window = 0x0010,
            .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
            .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
            .latency = 0, .supervision_timeout = 0x0100,
            .min_ce_len = 0x0010, .max_ce_len = 0x0300,
        };
        uint8_t own_addr_type;
        if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
            rhc_start_scan();
            return 0;
        }
        int rc = ble_gap_connect(own_addr_type, &event->disc.addr,
                                 30000, &conn_params, rhc_gap_event,
                                 NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "connect failed rc=%d, rescan", rc);
            rhc_start_scan();
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            rhc_conn_handle = event->connect.conn_handle;
            rhc_cmd_handle = rhc_state_handle = rhc_cccd_handle = 0;
            rhc_svc_start_handle = rhc_svc_end_handle = 0;
            ESP_LOGI(TAG, "connected, handle %u, securing + discovering",
                     rhc_conn_handle);
            /* Bond (LE SC, Just Works). On a bonded peer this
               restores encryption silently. */
            ble_gap_security_initiate(rhc_conn_handle);
            ble_uuid_any_t svc_uuid;
            ble_uuid_from_str(&svc_uuid, RHC_SVC_UUID_STR);
            int rc = ble_gattc_disc_svc_by_uuid(rhc_conn_handle,
                                                &svc_uuid.u,
                                                rhc_svc_disc_cb, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "service discovery failed rc=%d, "
                         "disconnecting", rc);
                ble_gap_terminate(rhc_conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d, rescan",
                     event->connect.status);
            rhc_start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d, rescan",
                 event->disconnect.reason);
        rhc_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        rhc_cmd_handle = rhc_state_handle = rhc_cccd_handle = 0;
        xEventGroupClearBits(rhc_events, RHC_READY_BIT);
        rhc_start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == rhc_state_handle) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len >= sizeof(rhc_receipt)) {
                len = sizeof(rhc_receipt) - 1;
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, rhc_receipt);
            rhc_receipt[len] = '\0';
            ESP_LOGI(TAG, "state notify: %s", rhc_receipt);
            xEventGroupSetBits(rhc_events, RHC_RECEIPT_BIT);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d",
                 event->enc_change.status);
        return 0;
    default:
        return 0;
    }
}

/* --- Public API --- */

void ble_central_init(void)
{
    rhc_events = xEventGroupCreate();
    rhc_exec_mutex = xSemaphoreCreateMutex();
    ble_hs_cfg.reset_cb = rhc_on_reset;
    ble_hs_cfg.sync_cb = rhc_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed rc=%d", rc);
        return;
    }
    /* Bond store: the NimBLE host initialises its config store
       itself (NVS-backed when CONFIG_BT_NIMBLE_NVS_PERSIST is on),
       so bonds survive reboots on both sides. No app call needed. */
    nimble_port_freertos_init(rhc_host_task);
    ESP_LOGI(TAG, "BLE central init done, scanning on host sync");
}

static int rhc_write_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "command write failed status=%d", error->status);
    }
    return 0;
}

bool ble_central_execute(const char *x_command)
{
    /* Parse "device:action[:param]". v1 has one device; anything
       else is NOT transmitted (a physical action must never be
       guessed). */
    if (x_command == NULL) return false;
    const char *c1 = strchr(x_command, ':');
    if (c1 == NULL || c1 == x_command) return false;
    char dev[32];
    size_t dev_len = (size_t)(c1 - x_command);
    if (dev_len >= sizeof(dev)) return false;
    memcpy(dev, x_command, dev_len);
    dev[dev_len] = '\0';
    if (strcmp(dev, "livingroom_light") != 0) {
        ESP_LOGW(TAG, "unknown device '%s', not sent", dev);
        return false;
    }
    const char *rest = c1 + 1;
    const char *c2 = strchr(rest, ':');
    char act[16];
    size_t act_len = c2 ? (size_t)(c2 - rest) : strlen(rest);
    if (act_len == 0 || act_len >= sizeof(act)) return false;
    memcpy(act, rest, act_len);
    act[act_len] = '\0';
    const char *param = c2 ? c2 + 1 : "";

    char ble_text[24];
    const char *expect;
    if (strcmp(act, "on") == 0) {
        strcpy(ble_text, "ON");
        expect = "ON";
    } else if (strcmp(act, "off") == 0) {
        strcpy(ble_text, "OFF");
        expect = "OFF";
    } else if (strcmp(act, "bri") == 0) {
        if (param[0] == '\0') return false;
        int n = atoi(param);
        snprintf(ble_text, sizeof(ble_text), "BRI:%d", n);
        /* Relay: any level > 0 means ON (actuator semantics). */
        expect = (n > 0) ? "ON" : "OFF";
    } else {
        ESP_LOGW(TAG, "unknown action '%s', not sent", act);
        return false;
    }

    if (xSemaphoreTake(rhc_exec_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if ((xEventGroupGetBits(rhc_events) & RHC_READY_BIT) == 0 ||
        rhc_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        rhc_cmd_handle == 0) {
        ESP_LOGW(TAG, "actuator not ready, command '%s' dropped",
                 ble_text);
        goto done;
    }
    xEventGroupClearBits(rhc_events, RHC_RECEIPT_BIT);
    rhc_receipt[0] = '\0';
    int rc = ble_gattc_write_flat(rhc_conn_handle, rhc_cmd_handle,
                                  ble_text, strlen(ble_text),
                                  rhc_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "command write failed rc=%d", rc);
        goto done;
    }
    EventBits_t bits = xEventGroupWaitBits(rhc_events, RHC_RECEIPT_BIT,
                                           pdTRUE, pdFALSE,
                                           RHC_RECEIPT_TIMEOUT_MS /
                                               portTICK_PERIOD_MS);
    if ((bits & RHC_RECEIPT_BIT) == 0) {
        ESP_LOGW(TAG, "receipt timeout %d ms for '%s'",
                 RHC_RECEIPT_TIMEOUT_MS, ble_text);
        goto done;
    }
    if (strcmp(rhc_receipt, expect) == 0) {
        ESP_LOGI(TAG, "command '%s' confirmed by actuator (%s)",
                 ble_text, rhc_receipt);
        ok = true;
    } else {
        ESP_LOGW(TAG, "receipt mismatch: got '%s', want '%s'",
                 rhc_receipt, expect);
    }
done:
    xSemaphoreGive(rhc_exec_mutex);
    return ok;
}
