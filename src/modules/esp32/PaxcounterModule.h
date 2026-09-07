#pragma once

#include "ProtobufModule.h"
#include "configuration.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_PAXCOUNTER
#include "../mesh/generated/meshtastic/paxcount.pb.h"
#include "NodeDB.h"
#include <libpax_api.h>

/**
 * Wrapper module for the estimate passenger (PAX) count library (https://github.com/dbinfrago/libpax) which
 * implements the core functionality of the ESP32 Paxcounter project (https://github.com/cyberman54/ESP32-Paxcounter)
 */
class PaxcounterModule : private concurrency::OSThread,
                         public ProtobufModule<meshtastic_Paxcount>,
                         public Observable<const UIFrameEvent *>
{
    static constexpr size_t MAX_SIGHTINGS = 64;
    static constexpr size_t SIGHTINGS_PER_CHUNK = 8; // matches Paxcount.sightings max_count
    static constexpr size_t MAX_UI_SIGHTINGS = 16;   // strongest IDs kept for the OLED view
    static constexpr uint32_t UI_PAGE_MS = 4000;     // auto-advance rows on buttonless boards

    struct SightingEntry {
        uint8_t mac[6];
        meshtastic_PaxSighting_Kind kind;
        int32_t rssi;
        uint8_t fingerprint[4];
        uint8_t fingerprint_len;
    };

    bool firstTime = true;
    bool reportedDataSent = true;

    SightingEntry sightings[MAX_SIGHTINGS] = {};
    size_t sightingCount = 0;
    portMUX_TYPE sightingsMux = portMUX_INITIALIZER_UNLOCKED;

    // Rolling strongest-signal table for the display (not cleared on mesh TX).
    SightingEntry uiSightings[MAX_UI_SIGHTINGS] = {};
    size_t uiSightingCount = 0;
    portMUX_TYPE uiSightingsMux = portMUX_INITIALIZER_UNLOCKED;

    static void handlePaxCounterReportRequest();
    static void handleMacSeen(const uint8_t mac[6], int rssi, int kind, const uint8_t *adv_data, uint8_t adv_len);
    static void upsertUiSighting(const SightingEntry &incoming);

    void fillCounts(meshtastic_Paxcount &pl) const;
    bool sendChunk(NodeNum dest, const meshtastic_Paxcount &pl);
    void refreshPaxScreen(bool forceFocus);

  public:
    PaxcounterModule();

  protected:
    struct count_payload_t count_from_libpax = {0, 0, 0};
    virtual int32_t runOnce() override;
    bool sendInfo(NodeNum dest = NODENUM_BROADCAST);
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Paxcount *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    bool isActive() { return moduleConfig.paxcounter.enabled && !config.bluetooth.enabled && !config.network.wifi_enabled; }
#if HAS_SCREEN
    virtual bool wantUIFrame() override { return isActive(); }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif
};

extern PaxcounterModule *paxcounterModule;
#endif
