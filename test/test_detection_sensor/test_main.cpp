// DetectionSensor dwell confirm + minimum_detect_secs defaults.
#include "Arduino.h"
#include "TestUtil.h"
#include "mesh-pb-constants.h"
#include "mesh/generated/meshtastic/module_config.pb.h"
#include "modules/DetectionSensorDwell.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_minimum_detect_secs_defaults_zero(void)
{
    meshtastic_ModuleConfig_DetectionSensorConfig cfg = meshtastic_ModuleConfig_DetectionSensorConfig_init_default;
    TEST_ASSERT_EQUAL_UINT32(0, cfg.minimum_detect_secs);
}

void test_minimum_detect_secs_roundtrips(void)
{
    meshtastic_ModuleConfig_DetectionSensorConfig cfg = meshtastic_ModuleConfig_DetectionSensorConfig_init_zero;
    cfg.enabled = true;
    cfg.minimum_broadcast_secs = 45;
    cfg.minimum_detect_secs = 2;
    cfg.monitor_pin = 21;

    uint8_t buf[64];
    size_t n = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_ModuleConfig_DetectionSensorConfig_msg, &cfg);
    TEST_ASSERT_TRUE(n > 0);

    meshtastic_ModuleConfig_DetectionSensorConfig decoded = meshtastic_ModuleConfig_DetectionSensorConfig_init_zero;
    TEST_ASSERT_TRUE(pb_decode_from_bytes(buf, n, &meshtastic_ModuleConfig_DetectionSensorConfig_msg, &decoded));
    TEST_ASSERT_EQUAL_UINT32(2, decoded.minimum_detect_secs);
    TEST_ASSERT_TRUE(decoded.enabled);
    TEST_ASSERT_EQUAL_UINT32(45, decoded.minimum_broadcast_secs);
}

void test_dwell_immediate_when_zero(void)
{
    bool armed = false;
    uint32_t started = 0;
    TEST_ASSERT_TRUE(detectionSensorUpdateDwell(true, 0, 1000, armed, started));
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(false, 0, 1000, armed, started));
}

void test_dwell_requires_hold_then_confirms(void)
{
    bool armed = false;
    uint32_t started = 0;

    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 1000, armed, started));
    TEST_ASSERT_TRUE(armed);
    TEST_ASSERT_EQUAL_UINT32(1000, started);

    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 2500, armed, started));
    TEST_ASSERT_TRUE(detectionSensorUpdateDwell(true, 2, 3000, armed, started));

    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(false, 2, 3100, armed, started));
    TEST_ASSERT_FALSE(armed);
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 3200, armed, started));
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 4000, armed, started));
    TEST_ASSERT_TRUE(detectionSensorUpdateDwell(true, 2, 5200, armed, started));
}

void test_dwell_glitch_resets_timer(void)
{
    bool armed = false;
    uint32_t started = 0;
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 0, armed, started));
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(false, 2, 500, armed, started));
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 600, armed, started));
    TEST_ASSERT_FALSE(detectionSensorUpdateDwell(true, 2, 2500, armed, started));
    TEST_ASSERT_TRUE(detectionSensorUpdateDwell(true, 2, 2600, armed, started));
}

void test_dwell_elapsed_ms(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, detectionSensorDwellElapsedMs(false, 1000, 3000));
    TEST_ASSERT_EQUAL_UINT32(2000, detectionSensorDwellElapsedMs(true, 1000, 3000));
}

void test_active_ms_spans_full_presence(void)
{
    // Pin high at t=1000, confirms after 2s dwell, clears at t=5500 → 4500ms presence.
    TEST_ASSERT_EQUAL_UINT32(1000, detectionSensorEpisodeStartMs(2, 1000, 1000));
    TEST_ASSERT_EQUAL_UINT32(4500, detectionSensorActiveMs(1000, 5500));
    // No dwell: episode starts at first pin-active sample.
    TEST_ASSERT_EQUAL_UINT32(900, detectionSensorEpisodeStartMs(0, 0, 900));
    TEST_ASSERT_EQUAL_UINT32(1100, detectionSensorActiveMs(900, 2000));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_minimum_detect_secs_defaults_zero);
    RUN_TEST(test_minimum_detect_secs_roundtrips);
    RUN_TEST(test_dwell_immediate_when_zero);
    RUN_TEST(test_dwell_requires_hold_then_confirms);
    RUN_TEST(test_dwell_glitch_resets_timer);
    RUN_TEST(test_dwell_elapsed_ms);
    RUN_TEST(test_active_ms_spans_full_presence);
    exit(UNITY_END());
}

void loop() {}
