#include "configuration.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_PAXCOUNTER
#include "Default.h"
#include "MeshService.h"
#include "PaxcounterModule.h"
#include "PowerFSM.h"
#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#endif
#include "graphics/images.h"
#include <esp_event.h>
#include <freertos/timers.h>
#include <string.h>

// Arduino WiFi has a wifiscan.h too, so declare the libpax scanner hooks here.
void set_wifi_channels(uint16_t channels_map);
void wifi_sniffer_init(uint16_t wifi_channel_switch_interval);
void switchWifiChannel(TimerHandle_t xTimer);
extern TimerHandle_t WifiChanTimer;

static void startWifiChannelTimer(uint16_t wifi_channel_switch_interval)
{
    if (wifi_channel_switch_interval == 0) {
        return;
    }

    WifiChanTimer =
        xTimerCreate("WifiChannelTimer", pdMS_TO_TICKS(wifi_channel_switch_interval * 10), pdTRUE, (void *)0, switchWifiChannel);
    if (!WifiChanTimer) {
        LOG_WARN("Paxcounter can't create WiFi channel switch timer");
        return;
    }
    xTimerStart(WifiChanTimer, 0);
}

static void ensureDefaultEventLoop()
{
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        LOG_WARN("Paxcounter can't create ESP event loop: %d", result);
    }
}

PaxcounterModule *paxcounterModule;

static uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

// Classify BLE adv payload: Apple/Android hints + soft fingerprint from mfg data.
static void classifyBleAdv(const uint8_t *adv, uint8_t adv_len, meshtastic_PaxSighting_Kind *kindOut, uint8_t fpOut[4],
                           uint8_t *fpLenOut)
{
    *kindOut = meshtastic_PaxSighting_Kind_BLE;
    *fpLenOut = 0;
    if (!adv || !adv_len)
        return;

    size_t i = 0;
    while (i < adv_len) {
        uint8_t alen = adv[i];
        if (alen == 0 || i + 1 + alen > adv_len)
            break;
        uint8_t type = adv[i + 1];
        const uint8_t *data = &adv[i + 2];
        uint8_t dlen = alen - 1;

        if (type == 0xFF && dlen >= 2) { // Manufacturer Specific Data
            uint16_t company = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
            if (company == 0x004C) {
                *kindOut = meshtastic_PaxSighting_Kind_BLE_APPLE;
            } else if (company == 0x00E0 || company == 0x0075 || company == 0x0006) {
                // Google Fast Pair / Samsung / Microsoft (common phone OEMs)
                *kindOut = meshtastic_PaxSighting_Kind_BLE_ANDROID;
            }
            if (dlen >= 3 && *fpLenOut == 0) {
                uint32_t h = fnv1a32(data, dlen);
                fpOut[0] = (uint8_t)(h >> 24);
                fpOut[1] = (uint8_t)(h >> 16);
                fpOut[2] = (uint8_t)(h >> 8);
                fpOut[3] = (uint8_t)h;
                *fpLenOut = 4;
            }
        } else if ((type == 0x16 || type == 0x21) && dlen >= 2 && *kindOut == meshtastic_PaxSighting_Kind_BLE) {
            // Service Data - Google exposure/nearby-ish UUIDs often on Android
            uint16_t uuid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
            if (uuid == 0xFE2C || uuid == 0xFE9F || uuid == 0xFCF1) {
                *kindOut = meshtastic_PaxSighting_Kind_BLE_ANDROID;
                if (dlen >= 3 && *fpLenOut == 0) {
                    uint32_t h = fnv1a32(data, dlen);
                    fpOut[0] = (uint8_t)(h >> 24);
                    fpOut[1] = (uint8_t)(h >> 16);
                    fpOut[2] = (uint8_t)(h >> 8);
                    fpOut[3] = (uint8_t)h;
                    *fpLenOut = 4;
                }
            }
        }
        i += (size_t)alen + 1;
    }
}

void PaxcounterModule::upsertUiSighting(const SightingEntry &incoming)
{
    if (!paxcounterModule)
        return;

    portENTER_CRITICAL(&paxcounterModule->uiSightingsMux);
    for (size_t i = 0; i < paxcounterModule->uiSightingCount; i++) {
        SightingEntry &e = paxcounterModule->uiSightings[i];
        bool same = false;
        if (incoming.fingerprint_len && e.fingerprint_len == incoming.fingerprint_len &&
            memcmp(e.fingerprint, incoming.fingerprint, incoming.fingerprint_len) == 0) {
            same = true;
        } else if (e.kind == incoming.kind && memcmp(e.mac, incoming.mac, 6) == 0) {
            same = true;
        }
        if (same) {
            memcpy(e.mac, incoming.mac, 6);
            e.kind = incoming.kind;
            if (incoming.rssi > e.rssi)
                e.rssi = incoming.rssi;
            if (incoming.fingerprint_len) {
                memcpy(e.fingerprint, incoming.fingerprint, incoming.fingerprint_len);
                e.fingerprint_len = incoming.fingerprint_len;
            }
            portEXIT_CRITICAL(&paxcounterModule->uiSightingsMux);
            return;
        }
    }

    if (paxcounterModule->uiSightingCount < MAX_UI_SIGHTINGS) {
        paxcounterModule->uiSightings[paxcounterModule->uiSightingCount++] = incoming;
    } else {
        // Replace the weakest entry if this signal is stronger.
        size_t weakest = 0;
        for (size_t i = 1; i < MAX_UI_SIGHTINGS; i++) {
            if (paxcounterModule->uiSightings[i].rssi < paxcounterModule->uiSightings[weakest].rssi)
                weakest = i;
        }
        if (incoming.rssi > paxcounterModule->uiSightings[weakest].rssi)
            paxcounterModule->uiSightings[weakest] = incoming;
    }
    portEXIT_CRITICAL(&paxcounterModule->uiSightingsMux);
}

void PaxcounterModule::handleMacSeen(const uint8_t mac[6], int rssi, int kind, const uint8_t *adv_data, uint8_t adv_len)
{
    if (!paxcounterModule)
        return;

    meshtastic_PaxSighting_Kind k = meshtastic_PaxSighting_Kind_WIFI_CLIENT;
    uint8_t fp[4] = {};
    uint8_t fpLen = 0;
    if (kind == LIBPAX_MAC_KIND_WIFI_AP)
        k = meshtastic_PaxSighting_Kind_WIFI_AP;
    else if (kind == LIBPAX_MAC_KIND_BLE)
        classifyBleAdv(adv_data, adv_len, &k, fp, &fpLen);

    SightingEntry incoming = {};
    memcpy(incoming.mac, mac, 6);
    incoming.kind = k;
    incoming.rssi = rssi;
    incoming.fingerprint_len = fpLen;
    if (fpLen)
        memcpy(incoming.fingerprint, fp, fpLen);

    // Always keep a live UI table (display works even if mesh report_ids is off).
    upsertUiSighting(incoming);

    if (!moduleConfig.paxcounter.report_ids)
        return;

    portENTER_CRITICAL(&paxcounterModule->sightingsMux);
    for (size_t i = 0; i < paxcounterModule->sightingCount; i++) {
        SightingEntry &e = paxcounterModule->sightings[i];
        bool same = false;
        if (fpLen && e.fingerprint_len == fpLen && memcmp(e.fingerprint, fp, fpLen) == 0) {
            same = true;
        } else if (e.kind == k && memcmp(e.mac, mac, 6) == 0) {
            same = true;
        }
        if (same) {
            memcpy(e.mac, mac, 6);
            e.kind = k;
            if (rssi > e.rssi)
                e.rssi = rssi;
            if (fpLen) {
                memcpy(e.fingerprint, fp, fpLen);
                e.fingerprint_len = fpLen;
            }
            portEXIT_CRITICAL(&paxcounterModule->sightingsMux);
            return;
        }
    }
    if (paxcounterModule->sightingCount < MAX_SIGHTINGS) {
        SightingEntry &e = paxcounterModule->sightings[paxcounterModule->sightingCount++];
        e = incoming;
    }
    portEXIT_CRITICAL(&paxcounterModule->sightingsMux);
}

/**
 * Callback function for libpax.
 * We only clear our sent flag here, since this function is called from another thread, so we
 * cannot send to the mesh directly.
 */
void PaxcounterModule::handlePaxCounterReportRequest()
{
    // The libpax library already updated our data structure, just before invoking this callback.
    LOG_INFO("PaxcounterModule: libpax reported new data: wifi=%d; ble=%d; uptime=%lu",
             paxcounterModule->count_from_libpax.wifi_count, paxcounterModule->count_from_libpax.ble_count, millis() / 1000);
    paxcounterModule->reportedDataSent = false;
    paxcounterModule->setIntervalFromNow(0);
}

PaxcounterModule::PaxcounterModule()
    : concurrency::OSThread("Paxcounter"),
      ProtobufModule("paxcounter", meshtastic_PortNum_PAXCOUNTER_APP, &meshtastic_Paxcount_msg)
{
}

void PaxcounterModule::fillCounts(meshtastic_Paxcount &pl) const
{
    pl.wifi = count_from_libpax.wifi_count;
    pl.ble = count_from_libpax.ble_count;
    pl.uptime = millis() / 1000;
}

bool PaxcounterModule::sendChunk(NodeNum dest, const meshtastic_Paxcount &pl)
{
    meshtastic_MeshPacket *p = allocDataProtobuf(pl);
    if (!p)
        return false;
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    service->sendToMesh(p, RX_SRC_LOCAL, true);
    return true;
}

/**
 * Send the Pax information to the mesh if we got new data from libpax.
 * This is called periodically from our runOnce() method and will actually send the data to the mesh
 * if libpax updated it since the last transmission through the callback.
 * @param dest - destination node (usually NODENUM_BROADCAST)
 * @return false if sending is unnecessary, true if information was sent
 */
bool PaxcounterModule::sendInfo(NodeNum dest)
{
    if (paxcounterModule->reportedDataSent)
        return false;

    LOG_INFO("PaxcounterModule: send pax info wifi=%d; ble=%d; uptime=%lu", count_from_libpax.wifi_count,
             count_from_libpax.ble_count, millis() / 1000);

    // Snapshot sightings under the lock so the sniffer can keep updating.
    SightingEntry local[MAX_SIGHTINGS];
    size_t localCount = 0;
    if (moduleConfig.paxcounter.report_ids) {
        portENTER_CRITICAL(&sightingsMux);
        localCount = sightingCount;
        memcpy(local, sightings, localCount * sizeof(SightingEntry));
        sightingCount = 0;
        portEXIT_CRITICAL(&sightingsMux);
    }

    if (!moduleConfig.paxcounter.report_ids || localCount == 0) {
        meshtastic_Paxcount pl = meshtastic_Paxcount_init_default;
        fillCounts(pl);
        if (!sendChunk(dest, pl))
            return false;
        paxcounterModule->reportedDataSent = true;
        return true;
    }

    const uint32_t chunkTotal = (uint32_t)((localCount + SIGHTINGS_PER_CHUNK - 1) / SIGHTINGS_PER_CHUNK);
    for (uint32_t chunk = 0; chunk < chunkTotal; chunk++) {
        meshtastic_Paxcount pl = meshtastic_Paxcount_init_default;
        fillCounts(pl);
        pl.sighting_count = (uint32_t)localCount;
        pl.chunk_index = chunk;
        pl.chunk_total = chunkTotal;

        const size_t start = (size_t)chunk * SIGHTINGS_PER_CHUNK;
        const size_t n = (localCount - start > SIGHTINGS_PER_CHUNK) ? SIGHTINGS_PER_CHUNK : (localCount - start);
        pl.sightings_count = (pb_size_t)n;
        for (size_t i = 0; i < n; i++) {
            memcpy(pl.sightings[i].mac, local[start + i].mac, 6);
            pl.sightings[i].kind = local[start + i].kind;
            pl.sightings[i].rssi = local[start + i].rssi;
            pl.sightings[i].fingerprint.size = local[start + i].fingerprint_len;
            if (local[start + i].fingerprint_len)
                memcpy(pl.sightings[i].fingerprint.bytes, local[start + i].fingerprint, local[start + i].fingerprint_len);
        }

        if (!sendChunk(dest, pl))
            return false;
    }

    paxcounterModule->reportedDataSent = true;
    return true;
}

bool PaxcounterModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Paxcount *p)
{
    return false; // Let others look at this message also if they want. We don't do anything with received packets.
}

meshtastic_MeshPacket *PaxcounterModule::allocReply()
{
    meshtastic_Paxcount pl = meshtastic_Paxcount_init_default;
    fillCounts(pl);
    return allocDataProtobuf(pl);
}

int32_t PaxcounterModule::runOnce()
{
    if (isActive()) {
        if (firstTime) {
            firstTime = false;
            LOG_DEBUG("Paxcounter starting up with interval of %d seconds",
                      Default::getConfiguredOrDefault(moduleConfig.paxcounter.paxcounter_update_interval,
                                                      default_telemetry_broadcast_interval_secs));
            struct libpax_config_t configuration;
            libpax_default_config(&configuration);

            configuration.blecounter = 1;
            configuration.blescantime = 0; // infinite
            configuration.wificounter = 1;
            configuration.LIBPAX_WIFI_CHANNEL_map = LIBPAX_WIFI_CHANNEL_ALL;
            configuration.LIBPAX_WIFI_CHANNEL_switch_interval = 50;
            configuration.wifi_rssi_threshold = Default::getConfiguredOrDefault(moduleConfig.paxcounter.wifi_threshold, -80);
            configuration.ble_rssi_threshold = Default::getConfiguredOrDefault(moduleConfig.paxcounter.ble_threshold, -80);
            libpax_update_config(&configuration);

            // Collect IDs for the OLED whenever a screen may be present; mesh TX still gated by report_ids.
            libpax_set_mac_callback(handleMacSeen);
            if (moduleConfig.paxcounter.report_ids) {
                LOG_INFO("PaxcounterModule: report_ids enabled, collecting WiFi/BLE MAC sightings");
            } else {
                LOG_INFO("PaxcounterModule: collecting WiFi/BLE MAC sightings for display only");
            }

            // internal processing initialization
            libpax_counter_init(handlePaxCounterReportRequest, &count_from_libpax,
                                Default::getConfiguredOrDefault(moduleConfig.paxcounter.paxcounter_update_interval,
                                                                default_telemetry_broadcast_interval_secs),
                                0);
            // libpax sets the WiFi country in counter_start(), so start channel rotation after that.
            ensureDefaultEventLoop();
            set_wifi_channels(configuration.LIBPAX_WIFI_CHANNEL_map);
            wifi_sniffer_init(0);
            libpax_counter_start();
            startWifiChannelTimer(configuration.LIBPAX_WIFI_CHANNEL_switch_interval);
            refreshPaxScreen(true); // show Pax frame on start (esp. buttonless boards)
        } else {
            if (sendInfo(NODENUM_BROADCAST))
                refreshPaxScreen(false);
        }
        return Default::getConfiguredOrDefaultMsScaled(moduleConfig.paxcounter.paxcounter_update_interval,
                                                       default_telemetry_broadcast_interval_secs, numOnlineNodes);
    } else {
        return disable();
    }
}

void PaxcounterModule::refreshPaxScreen(bool forceFocus)
{
#if HAS_SCREEN
    if (!isActive())
        return;

    // Boards without a user button (e.g. RAK3112) can't navigate frames manually.
    // On each pax interval: wake the panel and jump to the Pax frame.
    bool hasButton =
#if defined(BUTTON_PIN)
        true;
#else
        config.device.button_gpio != 0;
#endif
    if (!hasButton || forceFocus) {
        if (!hasButton)
            powerFSM.trigger(EVENT_INPUT);
        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
    } else {
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REDRAW_ONLY;
        notifyObservers(&e);
    }
#else
    (void)forceFocus;
#endif
}

#if HAS_SCREEN

static const char *kindLabel(meshtastic_PaxSighting_Kind kind)
{
    switch (kind) {
    case meshtastic_PaxSighting_Kind_WIFI_AP:
        return "AP";
    case meshtastic_PaxSighting_Kind_BLE_APPLE:
        return "iOS";
    case meshtastic_PaxSighting_Kind_BLE_ANDROID:
        return "And";
    case meshtastic_PaxSighting_Kind_BLE:
        return "BLE";
    case meshtastic_PaxSighting_Kind_WIFI_CLIENT:
    default:
        return "WiFi";
    }
}

void PaxcounterModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    graphics::drawCommonHeader(display, x, y, "Pax");

    libpax_counter_count(&count_from_libpax);

    char buffer[64];
    const int *rows = graphics::getTextPositions(display);
    snprintf(buffer, sizeof(buffer), "WiFi:%u  BLE:%u", count_from_libpax.wifi_count, count_from_libpax.ble_count);
    display->drawString(x, rows[line++], buffer);

    SightingEntry local[MAX_UI_SIGHTINGS];
    size_t n = 0;
    portENTER_CRITICAL(&uiSightingsMux);
    n = uiSightingCount;
    memcpy(local, uiSightings, n * sizeof(SightingEntry));
    portEXIT_CRITICAL(&uiSightingsMux);

    for (size_t i = 1; i < n; i++) {
        SightingEntry key = local[i];
        size_t j = i;
        while (j > 0 && local[j - 1].rssi < key.rssi) {
            local[j] = local[j - 1];
            j--;
        }
        local[j] = key;
    }

    // getTextPositions() exposes indices 0..6 (header uses 0).
    int availableRows = 0;
    for (int i = line; i <= 6; i++) {
        if (rows[i] + FONT_HEIGHT_SMALL < display->getHeight() - FONT_HEIGHT_SMALL)
            availableRows++;
        else
            break;
    }
    if (availableRows < 1)
        availableRows = 1;

    if (n == 0) {
        display->drawString(x, rows[line], "Scanning IDs...");
        graphics::drawCommonFooter(display, x, y);
        return;
    }

    const size_t pageSize = (size_t)availableRows;
    const size_t pageCount = (n + pageSize - 1) / pageSize;
    const size_t page = (pageCount > 1) ? ((millis() / UI_PAGE_MS) % pageCount) : 0;
    const size_t start = page * pageSize;
    const size_t end = (start + pageSize < n) ? (start + pageSize) : n;

    for (size_t i = start; i < end && line <= 6; i++) {
        const SightingEntry &e = local[i];
        char id[20];
        if (e.fingerprint_len >= 4) {
            snprintf(id, sizeof(id), "%02x%02x%02x%02x", e.fingerprint[0], e.fingerprint[1], e.fingerprint[2], e.fingerprint[3]);
        } else {
            snprintf(id, sizeof(id), "%02x%02x%02x", e.mac[3], e.mac[4], e.mac[5]);
        }
        if (pageCount > 1 && i == start) {
            snprintf(buffer, sizeof(buffer), "%4ld %s %s %u/%u", (long)e.rssi, kindLabel(e.kind), id, (unsigned)(page + 1),
                     (unsigned)pageCount);
        } else {
            snprintf(buffer, sizeof(buffer), "%4ld %s %s", (long)e.rssi, kindLabel(e.kind), id);
        }
        display->drawString(x, rows[line++], buffer);
    }

    graphics::drawCommonFooter(display, x, y);
}
#endif // HAS_SCREEN

#endif
