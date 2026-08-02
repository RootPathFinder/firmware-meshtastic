#pragma once
#include "SinglePortModule.h"

class DetectionSensorModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    DetectionSensorModule() : SinglePortModule("detection", meshtastic_PortNum_DETECTION_SENSOR_APP), OSThread("DetectionSensor")
    {
    }

  protected:
    virtual int32_t runOnce() override;

  private:
    bool firstTime = true;
    uint32_t lastSentToMesh = 0;
    bool wasDetected = false;
    bool dwellArmed = false;
    uint32_t dwellStartedMs = 0;
    // Continuous pin-active tracking (independent of mesh TX throttle).
    bool pinWasActive = false;
    uint32_t pinActiveStartedMs = 0;
    bool confirmedEpisode = false;
    uint32_t episodeStartMs = 0;
    bool pendingClearReport = false;
    uint32_t pendingActiveMs = 0;
    void sendDetectionMessage();
    void sendClearedMessage(uint32_t activeMs);
    void sendCurrentStateMessage(bool state);
    bool pinIsActive();
};

extern DetectionSensorModule *detectionSensorModule;
