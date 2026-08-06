# Detection Sensor - driveway (RAK12013) settings

Tuned from RAK4631 + RAK12013 field tests. The radar OUT pin is a **fixed ~2.6-2.8s pulse** (sometimes two pulses per pass), not a continuous “object still present” signal. Firmware coalesces pulses with `burst_gap_secs` and filters blips with `minimum_detect_secs`.

Firmware with these fields: `2.8.0.22c8c67` and later on branch `cursor/detection-sensor-dwell-9b22`.

## Config fields

| Field                 | What it does                                                                                              |
| --------------------- | --------------------------------------------------------------------------------------------------------- |
| `minimum_detect_secs` | Pin must stay active this long before a burst starts. Primary **blip / glitch** filter.                   |
| `burst_gap_secs`      | Pin must stay _inactive_ this long before a burst ends. Merges radar retrigger gaps into one event.       |
| `minimum_alert_secs`  | Wall-clock time in the **same burst** before a mesh alert is sent. `0` = alert as soon as dwell confirms. |

Common extras:

```text
detection_sensor.enabled = true
detection_sensor.name = Driveway
detection_sensor.detection_trigger_type = RISING_EDGE
detection_sensor.minimum_broadcast_secs = 30   # cooldown between mesh alerts
detection_sensor.state_broadcast_secs = 0
```

`minimum_broadcast_secs` is the **alert cooldown**: after a mesh `detected`, further bursts
are ignored until that many seconds pass. The matching `cleared` for a sent alert is still
delivered so duration is available.

## Profile A - alert on walk-by **and** car (reject blips only)

Use when any real person/vehicle pass should alert; ignore sub-second junk.

```text
minimum_detect_secs = 1
burst_gap_secs      = 3
minimum_alert_secs  = 0
```

**Why:** Field walk-bys were ~2649-2727 ms per OUT pulse (often two pulses). `minimum_detect_secs=1` drops blips; `burst_gap_secs=3` should merge the double pulse into one `detected` / one `cleared`. Alert fires on first confirm.

**Expected log (after gap is set):**

```text
Driveway detected
Driveway cleared active_ms=… burst_ms=…
```

If you still see two `detected` lines for one walk, `burst_gap_secs` is still `0` or the OUT gap is &gt; 3 s - raise `burst_gap_secs` to `4` or `5`.

## Profile B - prefer car / longer presence; suppress casual walk-by

Use when a single ~2.7 s radar pulse (typical walk-by) should **not** alert, but a longer burst (vehicle / lingering motion with retriggers) should.

```text
minimum_detect_secs = 1
burst_gap_secs      = 3
minimum_alert_secs  = 5
```

**Why:** A normal walk-by burst is often ~2.6-2.8 s wall time → below 5 s → **clear only, no alert**. Sustained / multi-pulse motion that keeps the burst alive past 5 s → `Driveway detected burst_ms=…`.

Start at `5`; if cars still miss, lower to `4`. If walk-bys still alert, raise to `6`-`8` and confirm `burst_gap_secs` is merging pulses (check `burst_ms` on clear).

## Profile C - maximum blip rejection (still alert on normal walk)

```text
minimum_detect_secs = 2
burst_gap_secs      = 3
minimum_alert_secs  = 0
```

Requires a full ~2 s solid OUT before the burst starts. Slightly slower alert; stronger against chatter.

## Quick chooser

| Goal                                                       | Profile |
| ---------------------------------------------------------- | ------- |
| Driveway alarm: people **or** cars, ignore leaves/glitches | **A**   |
| Reduce walk-by noise; favor longer / vehicle-like bursts   | **B**   |
| Noisy install; still want walk-bys                         | **C**   |

## Setting the values

If the Meshtastic CLI knows the fields:

```bash
meshtastic --set detection_sensor.minimum_detect_secs 1
meshtastic --set detection_sensor.burst_gap_secs 3
meshtastic --set detection_sensor.minimum_alert_secs 0   # Profile A
# or: --set detection_sensor.minimum_alert_secs 5       # Profile B
```

If the CLI rejects unknown fields, set them with a protobuf helper against the device (firmware already understands tags 9-11).

## How to read the log

| Message                                   | Meaning                                                                              |
| ----------------------------------------- | ------------------------------------------------------------------------------------ |
| `Driveway detected`                       | Alert (`minimum_alert_secs=0`)                                                       |
| `Driveway detected burst_ms=N`            | Alert after persistence threshold                                                    |
| `Driveway cleared active_ms=A burst_ms=B` | Burst ended. `A` = time pin was high; `B` = wall time of whole burst (includes gaps) |

**Healthy coalescing:** one walk → one detect + one clear, and usually `burst_ms` ≥ `active_ms`.  
**Not coalesced:** `active_ms == burst_ms` on several clears a second apart - increase `burst_gap_secs`.

## Notes specific to RAK12013

- Default OUT hold is ~2 s hardware-side (observed ~2.6-2.8 s in testing).
- `active_ms` alone is a poor “object size” metric; use **burst coalescing + `minimum_alert_secs`** for persistence.
- Hardware options (C-TM hold time, R-GN range, retrigger jumper) still apply if software tuning is not enough.
