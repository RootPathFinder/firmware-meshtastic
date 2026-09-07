// see Bluetooth Specification v5.0
// e.g. https://www.mouser.it/pdfdocs/bluetooth-Core-v50.pdf

#include "blescan.h"
#include "libpax.h"

#ifndef BLESCANWINDOW
#define BLESCANWINDOW 80 // [milliseconds]
#endif

#ifndef BLESCANINTERVAL
#define BLESCANINTERVAL 80 // [milliseconds]
#endif

#ifndef TAG
#define TAG __FILE__
#endif

int initialized_ble = 0;
int ble_rssi_threshold = 0;

typedef struct {
    uint8_t *q_data;
    uint16_t q_data_len;
} host_rcv_data_t;

static uint8_t hci_cmd_buf[128];

static QueueHandle_t adv_queue;
static TaskHandle_t hci_eventprocessor;

/*
 * @brief: BT controller callback function, used to notify the upper layer that
 *         controller is ready to receive command
 */
static void controller_rcv_pkt_ready(void)
{
    // nothing to do here
}

/*
 * @brief: BT controller callback function to transfer data packet to the host
 */
static int host_rcv_pkt(uint8_t *data, uint16_t len)
{
    host_rcv_data_t send_data;
    uint8_t *data_pkt;
    /* Check second byte for HCI event. If event opcode is 0x0e, the event is
     * HCI Command Complete event. Since we have received "0x0e" event, we can
     * check for byte 4 for command opcode and byte 6 for it's return status. */
    if (data[1] == 0x0e) {
        if (data[6] != 0) {
            ESP_LOGE(TAG, "Event opcode 0x%02x fail with reason: 0x%02x.", data[4], data[6]);
            return ESP_FAIL;
        }
    }

    data_pkt = (uint8_t *)malloc(sizeof(uint8_t) * len);
    if (data_pkt == NULL) {
        ESP_LOGE(TAG, "Malloc data_pkt failed");
        return ESP_FAIL;
    }
    memcpy(data_pkt, data, len);
    send_data.q_data = data_pkt;
    send_data.q_data_len = len;
    if (xQueueSend(adv_queue, (void *)&send_data, (TickType_t)0) != pdTRUE) {
        ESP_LOGD(TAG, "Failed to enqueue advertising report. Queue full.");
        free(data_pkt);
    }
    return ESP_OK;
}

static esp_vhci_host_callback_t vhci_host_cb = {controller_rcv_pkt_ready, host_rcv_pkt};

static void emit_ble_sighting(const uint8_t mac[6], int rssi, const uint8_t *adv, uint8_t adv_len)
{
    if (ble_rssi_threshold && (rssi < ble_rssi_threshold))
        return;
    mac_add((uint8_t *)mac, MAC_SNIFF_BLE);
    if (libpax_mac_callback) {
        libpax_mac_callback(mac, rssi, LIBPAX_MAC_KIND_BLE, adv, adv_len);
    }
}

static void hci_cmd_send_reset(void)
{
    uint16_t sz = make_cmd_reset(hci_cmd_buf);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_set_evt_mask(void)
{
    /* Set bit 61 in event mask to enable LE Meta events. */
    uint8_t evt_mask[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20};
    uint16_t sz = make_cmd_set_evt_mask(hci_cmd_buf, evt_mask);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_le_set_event_mask(void)
{
    /* Enable common LE meta subevents including Extended Advertising Report (bit 12). */
    uint8_t le_mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint16_t sz = make_cmd_ble_set_event_mask(hci_cmd_buf, le_mask);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_ble_scan_params(void)
{
    uint8_t scan_type = 0x00; // passive
    uint16_t scan_interval = BLESCANINTERVAL * 1000 / 625;
    uint16_t scan_window = BLESCANWINDOW * 1000 / 625;
    uint8_t own_addr_type = 0x00;
    uint8_t filter_policy = 0x00;
    uint16_t sz = make_cmd_ble_set_scan_params(hci_cmd_buf, scan_type, scan_interval, scan_window, own_addr_type, filter_policy);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_ble_scan_start(void)
{
    uint8_t scan_enable = 0x01;
    uint8_t filter_duplicates = 0x00;
    uint16_t sz = make_cmd_ble_set_scan_enable(hci_cmd_buf, scan_enable, filter_duplicates);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
    ESP_LOGI(TAG, "BLE legacy scanning started");
}

static void hci_cmd_send_ble_ext_scan_params(void)
{
    uint8_t scan_type = 0x00; // passive
    uint16_t scan_interval = BLESCANINTERVAL * 1000 / 625;
    uint16_t scan_window = BLESCANWINDOW * 1000 / 625;
    uint16_t sz = make_cmd_ble_set_ext_scan_params(hci_cmd_buf, 0x00, 0x00, scan_type, scan_interval, scan_window);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_ble_ext_scan_start(void)
{
    /* Duration/Period 0 = continuous until disabled. */
    uint16_t sz = make_cmd_ble_set_ext_scan_enable(hci_cmd_buf, 0x01, 0x00, 0x0000, 0x0000);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
    ESP_LOGI(TAG, "BLE extended scanning started");
}

static void handle_legacy_adv_report(uint8_t *queue_data, uint16_t data_ptr)
{
    // Bluetooth Core Vol 4 Part E 7.7.65.2 LE Advertising Report
    uint8_t num_responses = queue_data[data_ptr++];
    if (num_responses == 0 || num_responses > 0x19)
        return;

    // skip Event_Type[i] and Address_Type[i]
    data_ptr += 2 * num_responses;

    uint8_t *addr = (uint8_t *)malloc(sizeof(uint8_t) * 6 * num_responses);
    uint8_t *dlen = (uint8_t *)malloc(num_responses);
    if (addr == NULL || dlen == NULL) {
        ESP_LOGE(TAG, "Malloc legacy adv failed");
        free(addr);
        free(dlen);
        return;
    }

    for (int i = 0; i < num_responses; i++) {
        for (int j = 5; j >= 0; j--) {
            addr[(6 * i) + j] = queue_data[data_ptr++];
        }
    }

    uint16_t total_data_len = 0;
    for (uint8_t i = 0; i < num_responses; i++) {
        dlen[i] = queue_data[data_ptr++];
        total_data_len += dlen[i];
    }

    const uint8_t *data_base = &queue_data[data_ptr];
    data_ptr += total_data_len;

    uint16_t data_off = 0;
    for (uint8_t i = 0; i < num_responses; i++) {
        short int rssi = -(0xFF - queue_data[data_ptr++]);
        emit_ble_sighting(addr + 6 * i, rssi, data_base + data_off, dlen[i]);
        data_off += dlen[i];
    }

    free(addr);
    free(dlen);
}

static void handle_ext_adv_report(uint8_t *queue_data, uint16_t data_ptr, uint16_t q_len)
{
    // Bluetooth Core Vol 4 Part E 7.7.65.13 LE Extended Advertising Report
    uint8_t num_reports = queue_data[data_ptr++];
    if (num_reports == 0 || num_reports > 0x19)
        return;

    for (uint8_t i = 0; i < num_reports; i++) {
        if (data_ptr + 24 > q_len) // min fixed fields before Data
            return;

        data_ptr += 2; // Event_Type
        data_ptr += 1; // Address_Type

        uint8_t mac[6];
        for (int j = 5; j >= 0; j--) {
            mac[j] = queue_data[data_ptr++];
        }

        data_ptr += 1; // Primary_PHY
        data_ptr += 1; // Secondary_PHY
        data_ptr += 1; // Advertising_SID
        data_ptr += 1; // TX_Power
        int8_t rssi_raw = (int8_t)queue_data[data_ptr++];
        data_ptr += 2; // Periodic_Advertising_Interval
        data_ptr += 1; // Direct_Address_Type
        data_ptr += 6; // Direct_Address

        if (data_ptr >= q_len)
            return;
        uint8_t adv_len = queue_data[data_ptr++];
        if (data_ptr + adv_len > q_len)
            return;

        const uint8_t *adv = &queue_data[data_ptr];
        data_ptr += adv_len;

        // 0x7F means RSSI unavailable
        if (rssi_raw == 0x7F)
            continue;
        emit_ble_sighting(mac, (int)rssi_raw, adv, adv_len);
    }
}

void hci_evt_process(void *pvParameters)
{
    host_rcv_data_t *rcv_data = (host_rcv_data_t *)malloc(sizeof(host_rcv_data_t));
    if (rcv_data == NULL) {
        ESP_LOGE(TAG, "Malloc rcv_data failed");
        return;
    }

    while (1) {
        if (xQueueReceive(adv_queue, rcv_data, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Queue receive error");
            continue;
        }

        uint8_t *queue_data = rcv_data->q_data;
        uint16_t q_len = rcv_data->q_data_len;
        uint16_t data_ptr = 0;

        // H4 packet: type, event code, ...
        uint8_t hci_event_opcode = queue_data[++data_ptr];
        if (hci_event_opcode == LE_META_EVENTS) {
            data_ptr += 2; // parameter total length + unused
            uint8_t sub_event = queue_data[data_ptr++];
            if (sub_event == HCI_LE_ADV_REPORT) {
                handle_legacy_adv_report(queue_data, data_ptr);
            } else if (sub_event == HCI_LE_EXT_ADV_REPORT) {
                handle_ext_adv_report(queue_data, data_ptr, q_len);
            }
        }
        free(queue_data);
    }
}

void start_BLE_scan(uint16_t blescantime, uint16_t blescanwindow, uint16_t blescaninterval)
{
#ifdef LIBPAX_BLE
    ESP_LOGI(TAG, "Initializing bluetooth scanner ...");

#ifdef LIBPAX_ARDUINO
    if (btStart()) {
#endif
#ifdef LIBPAX_ESPIDF
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
#endif

        adv_queue = xQueueCreate(30, sizeof(host_rcv_data_t));
        if (adv_queue == NULL) {
            ESP_LOGE(TAG, "Queue creation failed");
            return;
        }

        xTaskCreatePinnedToCore(&hci_evt_process, "hci_evt_process", 3072, NULL, 1, &hci_eventprocessor, 0);

        esp_vhci_host_register_callback(&vhci_host_cb);

        // Extended scan (BLE 5) reports both legacy and extended PDUs via
        // HCI_LE_EXT_ADV_REPORT. Classic ESP32 controllers stay on legacy scan.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) ||            \
    defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32H2) || defined(CONFIG_BT_BLE_50_FEATURES_SUPPORTED)
        const bool use_ext_scan = true;
#else
    const bool use_ext_scan = false;
#endif

        bool continue_commands = 1;
        int cmd_cnt = 0;

        while (continue_commands) {
            if (continue_commands && esp_vhci_host_check_send_available()) {
                switch (cmd_cnt) {
                case 0:
                    hci_cmd_send_reset();
                    ++cmd_cnt;
                    break;
                case 1:
                    hci_cmd_send_set_evt_mask();
                    ++cmd_cnt;
                    break;
                case 2:
                    if (use_ext_scan) {
                        hci_cmd_send_le_set_event_mask();
                    } else {
                        hci_cmd_send_ble_scan_params();
                    }
                    ++cmd_cnt;
                    break;
                case 3:
                    if (use_ext_scan) {
                        hci_cmd_send_ble_ext_scan_params();
                    } else {
                        hci_cmd_send_ble_scan_start();
                        continue_commands = 0;
                    }
                    ++cmd_cnt;
                    break;
                case 4:
                    hci_cmd_send_ble_ext_scan_start();
                    ++cmd_cnt;
                    break;
                default:
                    continue_commands = 0;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "Bluetooth scanner started");
        initialized_ble = 1;
#ifdef LIBPAX_ARDUINO
    } else {
        ESP_LOGE(TAG, "Failed on Bluetooth scanner started");
    }
#endif
#endif
} // start_BLEscan

void stop_BLE_scan(void)
{
#ifdef LIBPAX_BLE
    if (initialized_ble) {
        ESP_LOGI(TAG, "Shutting down bluetooth scanner ...");
#ifdef LIBPAX_ARDUINO
        btStop();
#endif
#ifdef LIBPAX_ESPIDF
        ESP_ERROR_CHECK(esp_bt_controller_disable());
        ESP_ERROR_CHECK(esp_bt_controller_deinit());
#endif
        ESP_LOGI(TAG, "Bluetooth scanner stopped");
        initialized_ble = 0;
    }
#endif
} // stop_BLEscan

void set_BLE_rssi_filter(int set_rssi_threshold)
{
    ble_rssi_threshold = set_rssi_threshold;
}
