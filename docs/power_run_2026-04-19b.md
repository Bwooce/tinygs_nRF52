# Power Measurement Run #2 — 2026-04-19 (attempt b)

Second attempt after the first run was invalidated by USB reconnect
at 16:51 during the DNS routing diagnosis.

## Firmware under test

- **Commit at flash:** `af424e7` (flashed 2026-04-19 ~17:27 local)
- **Vs run #1 firmware (`2518505`):** adds NAT64-synthesised DNS/SNTP
  targets. This avoids the ULA→global egress block that caused the
  OTBR-restart recovery to stall. Steady-state power should be within
  single-digit mA of run #1 — the new helpers run only on boot and on
  periodic SNTP re-sync.

## Battery

- **3000 mAh LiPo** (same cell as run #1).
- **Charging history:** was on USB from ~16:48 to ~[unplug time] on
  2026-04-19 for DNS debugging + flashing. Fully topped up.

## Baseline — captured 2026-04-19 18:01

User unplugged USB at ~17:36. Grabbed STATUS samples after 20 min
settle. Current boot only yielded 5 post-unplug samples (STATUS
fires every 5 min; a full 8-sample window would have been 40 min),
so accepting a narrower window than run #1.

| Time | Uptime | Vbat | lora_rx | Notes |
|---|---|---|---|---|
| 17:38:14 | 629 s  | 4078 mV | 8  | ~2 min post-unplug — may still be settling |
| 17:43:15 | 929 s  | 4104 mV | 28 | |
| 17:48:15 | 1230 s | 4087 mV | 36 | |
| 17:53:15 | 1530 s | 4078 mV | 44 | |
| 17:58:15 | 1830 s | 4069 mV | 46 | |

- **Mean: 4083 mV** (range 4069-4104, σ ≈ 13 mV)
- **Baseline timestamp: 17:58 local on 2026-04-19**

Sanity check: no reboots, no parent-lost/re-attached, no MQTT
disconnects since unplug. iot_log flowing live. lora_rx climbing
steadily (0 → 46 in first 30 min post-boot — much better pass
geometry than run #1 which was stuck at lora_rx=1).

## Firmware — post-baseline commits queued for next flash window

These commits are on main but not on the device. They'll all go out
together on the next flash (scheduled for after this run completes):

- `d498a53` Log TLE vs TLX in modem-config summary line
- `e6b90cd` Align rx_payload with TINYGS_BEGINE_MAX_LEN
- `f95d148` Log Thread attach/detach transitions with elapsed time
- `77dd5ef`, `416abe1`, `b120988` errno + OT + RadioLib error translation
- `fe4d2c3` get_utc_epoch rollover fix (now superseded by `3b09bb3`)
- `3b09bb3` Simplify SNTP offset to ms

None affect steady-state power draw; all are diagnostic improvements
that happen to slightly increase flash footprint (~5 KB total).

## Target

3-4 days. Come back 2026-04-22 or 2026-04-23.

## Invalidation conditions

(same rules as run #1 — see `power_run_2026-04-19.md`)

- Any USB contact (`./flash.sh` or otherwise)
- More than a handful of unexpected cold reboots
- iot_log silence gap > 12 h

## Periodic checks

Same compact format as run #1 §"Periodic status checks".
