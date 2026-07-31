# Meshtastic libpax fork notes

Vendored under `extra_lib/libpax` (kept out of `lib/` so native/portduino
builds do not auto-compile it) from
[mverch67/libpax](https://github.com/mverch67/libpax) at
`6f52ee989301cdabaeef00bcbf93bff55708ce2f`.

## Patches

- `libpax_set_mac_callback()` - optional callback for full WiFi MAC / BSSID
  sightings (used when `ModuleConfig.PaxcounterConfig.report_ids` is enabled).
- WiFi sniffer invokes the callback for client addresses and for AP BSSIDs
  (beacon / probe-response). The count path is unchanged (still only counts
  locally-administered MACs via the truncated bitmap).
