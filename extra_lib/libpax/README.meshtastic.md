# Meshtastic libpax fork notes

Vendored under `extra_lib/libpax` (kept out of `lib/` so native/portduino
builds do not auto-compile it) from
[mverch67/libpax](https://github.com/mverch67/libpax) at
`6f52ee989301cdabaeef00bcbf93bff55708ce2f`.

## Patches

- `libpax_set_mac_callback()` - optional callback for full WiFi MAC / BSSID
  and BLE address sightings (used when `ModuleConfig.PaxcounterConfig.report_ids`
  is enabled). Callback includes BLE advertisement payload when available.
- WiFi sniffer invokes the callback for client addresses and for AP BSSIDs
  (beacon / probe-response). BLE scan invokes it for advertiser addresses that
  pass the BLE RSSI filter. The count path is unchanged (still only counts
  locally-administered MACs via the truncated bitmap).
- On BLE 5 capable chips (ESP32-S3/C3/…), scanning uses HCI LE Extended Scan
  so both legacy and extended advertising PDUs are reported.

When a MAC is accepted after RSSI filtering, libpax may call this with `kind`:

- `0` (`LIBPAX_MAC_KIND_WIFI_CLIENT`) - WiFi station address
- `1` (`LIBPAX_MAC_KIND_WIFI_AP`) - AP BSSID
- `2` (`LIBPAX_MAC_KIND_BLE`) - BLE advertiser address

RSSI is the packet RSSI (dBm). Callbacks may fire for the same MAC more than once;
firmware deduplicates and may derive a soft fingerprint from BLE adv payloads.
