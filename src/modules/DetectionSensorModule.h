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
    void sendDetectionMessage();
    void sendCurrentStateMessage(bool state);
    bool pinIsActive();
};

extern DetectionSensorModule *detectionSensorModule;
