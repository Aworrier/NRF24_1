#include "ble_ctrl.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_CTRL_MAX_LINE 192

static const char *TAG = "ble_ctrl";

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);

static const ble_uuid128_t s_cmd_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);

static const ble_uuid128_t s_notify_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

typedef struct {
    bool initialized;
    bool notify_enabled;
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint16_t notify_val_handle;
    char device_name[32];
    ble_ctrl_command_cb_t command_cb;
    void *command_ctx;
} ble_ctrl_ctx_t;

static ble_ctrl_ctx_t s_ctx = {
    .initialized = false,
    .notify_enabled = false,
    .own_addr_type = BLE_OWN_ADDR_PUBLIC,
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .notify_val_handle = 0,
    .device_name = "ESP32-NRF24-BLE",
    .command_cb = NULL,
    .command_ctx = NULL,
};

static void ble_ctrl_advertise(void);

static int ble_ctrl_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    const ble_uuid_t *uuid = ctxt->chr->uuid;
    if (ble_uuid_cmp(uuid, &s_cmd_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
        if (pkt_len <= 0 || pkt_len >= BLE_CTRL_MAX_LINE) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        char line[BLE_CTRL_MAX_LINE] = {0};
        int rc = ble_hs_mbuf_to_flat(ctxt->om, line, sizeof(line) - 1, NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        for (int i = 0; i < pkt_len; ++i) {
            if (line[i] == '\r' || line[i] == '\n') {
                line[i] = '\0';
                break;
            }
        }

        if (s_ctx.command_cb != NULL) {
            s_ctx.command_cb(line, s_ctx.command_ctx);
        }
        return 0;
    }

    if (ble_uuid_cmp(uuid, &s_notify_uuid.u) == 0) {
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_cmd_uuid.u,
        .access_cb = ble_ctrl_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_notify_uuid.u,
        .access_cb = ble_ctrl_access_cb,
        .val_handle = &s_ctx.notify_val_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int ble_ctrl_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_ctx.conn_handle = event->connect.conn_handle;
                s_ctx.notify_enabled = false;
                ESP_LOGI(TAG, "BLE connected, handle=%u", (unsigned)s_ctx.conn_handle);
            } else {
                s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_ctx.notify_enabled = false;
                ble_ctrl_advertise();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
            s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ctx.notify_enabled = false;
            ble_ctrl_advertise();
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_ctx.notify_val_handle) {
                s_ctx.notify_enabled = (event->subscribe.cur_notify != 0);
            }
            ESP_LOGI(TAG, "BLE subscribe attr=%u cur_notify=%u", (unsigned)event->subscribe.attr_handle,
                     (unsigned)event->subscribe.cur_notify);
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_ctrl_advertise();
            return 0;
        default:
            return 0;
    }
}

static void ble_ctrl_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_ctx.device_name;
    fields.name_len = (uint8_t)strlen(s_ctx.device_name);
    fields.name_is_complete = 1;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_ctx.own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_ctrl_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising as %s", s_ctx.device_name);
    }
}

static void ble_ctrl_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_ctx.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    ble_ctrl_advertise();
}

static void ble_ctrl_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE reset, reason=%d", reason);
}

static void ble_ctrl_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

esp_err_t ble_ctrl_init(const char *device_name, ble_ctrl_command_cb_t command_cb, void *ctx)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    if (device_name != NULL && device_name[0] != '\0') {
        size_t cp = strlen(device_name);
        if (cp >= sizeof(s_ctx.device_name)) {
            cp = sizeof(s_ctx.device_name) - 1;
        }
        memcpy(s_ctx.device_name, device_name, cp);
        s_ctx.device_name[cp] = '\0';
    }

    s_ctx.command_cb = command_cb;
    s_ctx.command_ctx = ctx;

    ESP_RETURN_ON_FALSE(esp_nimble_hci_init() == ESP_OK, ESP_FAIL, TAG, "esp_nimble_hci_init failed");

    ESP_RETURN_ON_FALSE(nimble_port_init() == 0, ESP_FAIL, TAG, "nimble_port_init failed");

    ble_hs_cfg.reset_cb = ble_ctrl_on_reset;
    ble_hs_cfg.sync_cb = ble_ctrl_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "ble_gatts_count_cfg rc=%d", rc);

    rc = ble_gatts_add_svcs(s_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "ble_gatts_add_svcs rc=%d", rc);

    rc = ble_svc_gap_device_name_set(s_ctx.device_name);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "device_name_set rc=%d", rc);

    nimble_port_freertos_init(ble_ctrl_host_task);

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t ble_ctrl_send_line(const char *line)
{
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_INVALID_ARG, TAG, "line is null");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(s_ctx.conn_handle != BLE_HS_CONN_HANDLE_NONE, ESP_ERR_INVALID_STATE, TAG, "not connected");
    ESP_RETURN_ON_FALSE(s_ctx.notify_val_handle != 0, ESP_ERR_INVALID_STATE, TAG, "notify handle invalid");
    ESP_RETURN_ON_FALSE(s_ctx.notify_enabled, ESP_ERR_INVALID_STATE, TAG, "notify not enabled by client");

    char packet[BLE_CTRL_MAX_LINE] = {0};
    size_t len = strlen(line);
    if (len > BLE_CTRL_MAX_LINE - 3) {
        len = BLE_CTRL_MAX_LINE - 3;
    }
    memcpy(packet, line, len);
    packet[len++] = '\r';
    packet[len++] = '\n';

    struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, len);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_ctx.conn_handle, s_ctx.notify_val_handle, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ble_ctrl_is_connected(void)
{
    return s_ctx.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
