// Verifies Paxcount with a full sightings chunk encodes under DATA_PAYLOAD_LEN,
// and that report_ids defaults off in PaxcounterConfig.
#include "Arduino.h"
#include "TestUtil.h"
#include "mesh-pb-constants.h"
#include "mesh/generated/meshtastic/module_config.pb.h"
#include "mesh/generated/meshtastic/paxcount.pb.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_paxcount_max_size_fits_payload(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(meshtastic_Constants_DATA_PAYLOAD_LEN, meshtastic_Paxcount_size);
}

void test_paxcount_full_chunk_encodes_under_payload_len(void)
{
    meshtastic_Paxcount pl = meshtastic_Paxcount_init_zero;
    pl.wifi = 42;
    pl.ble = 17;
    pl.uptime = 123456;
    pl.sighting_count = 64;
    pl.chunk_index = 3;
    pl.chunk_total = 7;
    pl.sightings_count = 8;
    for (pb_size_t i = 0; i < pl.sightings_count; i++) {
        for (int b = 0; b < 6; b++)
            pl.sightings[i].mac[b] = (uint8_t)(i * 16 + b);
        static const meshtastic_PaxSighting_Kind kinds[] = {
            meshtastic_PaxSighting_Kind_WIFI_CLIENT, meshtastic_PaxSighting_Kind_WIFI_AP,     meshtastic_PaxSighting_Kind_BLE,
            meshtastic_PaxSighting_Kind_BLE_APPLE,   meshtastic_PaxSighting_Kind_BLE_ANDROID,
        };
        pl.sightings[i].kind = kinds[i % 5];
        pl.sightings[i].rssi = -40 - (int32_t)i;
        if (i >= 2) {
            pl.sightings[i].fingerprint.size = 4;
            pl.sightings[i].fingerprint.bytes[0] = (uint8_t)(0xA0 + i);
            pl.sightings[i].fingerprint.bytes[1] = 0x11;
            pl.sightings[i].fingerprint.bytes[2] = 0x22;
            pl.sightings[i].fingerprint.bytes[3] = 0x33;
        }
    }

    uint8_t buf[meshtastic_Constants_DATA_PAYLOAD_LEN];
    size_t n = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_Paxcount_msg, &pl);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(meshtastic_Constants_DATA_PAYLOAD_LEN, n);

    meshtastic_Paxcount decoded = meshtastic_Paxcount_init_zero;
    TEST_ASSERT_TRUE(pb_decode_from_bytes(buf, n, &meshtastic_Paxcount_msg, &decoded));
    TEST_ASSERT_EQUAL_UINT32(pl.wifi, decoded.wifi);
    TEST_ASSERT_EQUAL_UINT32(pl.ble, decoded.ble);
    TEST_ASSERT_EQUAL_UINT32(pl.sighting_count, decoded.sighting_count);
    TEST_ASSERT_EQUAL_UINT32(pl.chunk_index, decoded.chunk_index);
    TEST_ASSERT_EQUAL_UINT32(pl.chunk_total, decoded.chunk_total);
    TEST_ASSERT_EQUAL_UINT32(pl.sightings_count, decoded.sightings_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl.sightings[0].mac, decoded.sightings[0].mac, 6);
    TEST_ASSERT_EQUAL_INT32(pl.sightings[7].rssi, decoded.sightings[7].rssi);
    TEST_ASSERT_EQUAL_INT32(meshtastic_PaxSighting_Kind_BLE, decoded.sightings[2].kind);
    TEST_ASSERT_EQUAL_INT32(meshtastic_PaxSighting_Kind_BLE_APPLE, decoded.sightings[3].kind);
    TEST_ASSERT_EQUAL_UINT32(4, decoded.sightings[3].fingerprint.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl.sightings[3].fingerprint.bytes, decoded.sightings[3].fingerprint.bytes, 4);
}

void test_report_ids_defaults_false(void)
{
    meshtastic_ModuleConfig_PaxcounterConfig cfg = meshtastic_ModuleConfig_PaxcounterConfig_init_default;
    TEST_ASSERT_FALSE(cfg.report_ids);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_paxcount_max_size_fits_payload);
    RUN_TEST(test_paxcount_full_chunk_encodes_under_payload_len);
    RUN_TEST(test_report_ids_defaults_false);
    exit(UNITY_END());
}

void loop() {}
