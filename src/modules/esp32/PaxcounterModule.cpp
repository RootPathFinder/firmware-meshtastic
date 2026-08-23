#include "configuration.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_PAXCOUNTER
#include "Default.h"
#include "MeshService.h"
#include "PaxcounterModule.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
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

void PaxcounterModule::handleMacSeen(const uint8_t mac[6], int rssi, int kind)
{
    if (!paxcounterModule || !moduleConfig.paxcounter.report_ids)
        return;

    meshtastic_PaxSighting_Kind k = meshtastic_PaxSighting_Kind_WIFI_CLIENT;
    if (kind == LIBPAX_MAC_KIND_WIFI_AP)
        k = meshtastic_PaxSighting_Kind_WIFI_AP;
    else if (kind == LIBPAX_MAC_KIND_BLE)
        k = meshtastic_PaxSighting_Kind_BLE;

    portENTER_CRITICAL(&paxcounterModule->sightingsMux);
    for (size_t i = 0; i < paxcounterModule->sightingCount; i++) {
        SightingEntry &e = paxcounterModule->sightings[i];
        if (e.kind == k && memcmp(e.mac, mac, 6) == 0) {
            if (rssi > e.rssi)
                e.rssi = rssi;
            portEXIT_CRITICAL(&paxcounterModule->sightingsMux);
            return;
        }
    }
    if (paxcounterModule->sightingCount < MAX_SIGHTINGS) {
        SightingEntry &e = paxcounterModule->sightings[paxcounterModule->sightingCount++];
        memcpy(e.mac, mac, 6);
        e.kind = k;
        e.rssi = rssi;
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

            if (moduleConfig.paxcounter.report_ids) {
                libpax_set_mac_callback(handleMacSeen);
                LOG_INFO("PaxcounterModule: report_ids enabled, collecting WiFi/BLE MAC sightings");
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
        } else {
            sendInfo(NODENUM_BROADCAST);
        }
        return Default::getConfiguredOrDefaultMsScaled(moduleConfig.paxcounter.paxcounter_update_interval,
                                                       default_telemetry_broadcast_interval_secs, numOnlineNodes);
    } else {
        return disable();
    }
}

#if HAS_SCREEN

#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"

void PaxcounterModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    int line = 1;

    // === Set Title
    const char *titleStr = "Pax";

    // === Header ===
    graphics::drawCommonHeader(display, x, y, titleStr);

    char buffer[50];
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    libpax_counter_count(&count_from_libpax);

    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(FONT_SMALL);
    display->drawStringf(display->getWidth() / 2 + x, graphics::getTextPositions(display)[line++], buffer,
                         "WiFi: %d\nBLE: %d\nUptime: %ds", count_from_libpax.wifi_count, count_from_libpax.ble_count,
                         millis() / 1000);
    graphics::drawCommonFooter(display, x, y);
}
#endif // HAS_SCREEN

#endif
