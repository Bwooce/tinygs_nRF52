# NCS v2.6.0 → v3.3.0 Upgrade Plan (Path B)

**Status:** planning / scheduled for after Phase 3 (Device Web UI) lands and validates.
**Last updated:** 2026-04-24

This document is the delta analysis and migration plan for moving from our pinned
`v2.6.0 / Zephyr v3.5.99-ncs1` base to a current NCS release (target: v3.3.0, released
2026-04-23). The current stack has been deployed and validated through
multi-day battery runs; the upgrade is not urgent but is non-trivial and needs to be
budgeted as its own project phase rather than merged with feature work.

---

## 1. Why upgrade at all

Three real benefits, listed from "actually applies to us today" downward:

1. **OpenThread DNS-client bugfixes.** Upstream commits `4c45ee8e0` (Sep 2024) and
   `d163dee2a` (Oct 2024) fix the exact `otDnsClientResolveIp4Address` + NAT64 code path
   we use for every MQTT reconnect. The first repaired a backwards `QueryType` check
   in `Client::ReplaceWithIp4Query()` that silently sent wrong follow-up queries; the
   second added handling for "NOERROR but empty answer" responses that IPv4-only hosts
   (like `mqtt.tinygs.com`) commonly return. These are not backported to the v2.6 LTS
   line — v2.6.5's OpenThread is actually *older* (`7761b81d2`, Jan 2024) than ours
   (`b9dcdbca4`, Feb 2024), so the LTS track can't deliver them.

2. **Zephyr HTTP server maturity.** The in-tree HTTP server we plan to use for the
   Phase 3 device web UI (PLAN §21) has had substantial work landed between Zephyr
   v3.5 and v3.7. Concurrency limits, resource cleanup, and WebSocket support all
   improved. Our fallback plan was a hand-rolled HTTP/1.0 parser if the in-tree one
   proved unstable; upgrading may remove that fallback from the risk list entirely.

3. **Security fixes across ~25 months of mbedTLS / network stack / USB stack.**
   No specific CVE known to affect us, but a deployed device running TLS to a public
   MQTT broker should not accumulate that much lag indefinitely. This is "hygiene"
   reason, not "we have a known problem" reason.

Deliberately *not* in the benefits column:
- nRF54L / nRF54H20 / nRF91 / Matter / audio / BLE updates — none of our hardware.
- ZMS settings backend — our working set fits NVS just fine; opting into ZMS would
  be a lateral move with migration risk for no gain.
- PSA crypto as default — we run with `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` deliberately
  because the legacy RSA verify path was what actually handshake-compatible with
  mqtt.tinygs.com (see the painful Phase 1 investigation).

---

## 2. Our external API surface (what the upgrade can break)

Grepped from `src/*.cpp`, `src/hal/*.cpp`, and `prj.conf`. 177 distinct symbols; the
ones that *could* break across v2.6 → v3.3 are:

### OpenThread (~30 APIs)

Used by `main.cpp`, `tinygs_protocol.cpp`:
- `otDnsClientResolveIp4Address`, `otDnsAddressResponseGetAddress`, `otDnsQueryConfig`
- `otSntpClientQuery`, `otSntpQuery`
- `otJoinerStart`, `otDatasetIsCommissioned`, `otDatasetGetActive`
- `otThreadSetEnabled`, `otThreadGetDeviceRole`, `otThreadErrorToString`,
  `otThreadGetParentAverageRssi`
- `otLinkGetChannel`, `otLinkGetExtendedAddress`, `otLinkGetPanId`
- `otIp6SetEnabled`, `otIp6AddressToString`, `otNetifAddress`
- `otNetDataGetNextRoute`, `otNetworkDataIterator`, `otExternalRouteConfig`
- `otExtAddress`, `otMessageInfo`, `otInstance`
- `otError`, `otDeviceRole`, `otChangedFlags`
- `openthread_api_mutex_lock`/`unlock`, `openthread_get_default_context`,
  `openthread_start`, `openthread_state_changed_cb_register`

**Risk:** LOW for the OT public API — Google maintains a stable C ABI. The wrapper
APIs (`openthread_*`) are Zephyr-layer and more volatile. The v3.1 Thread migration
notes say OpenThread samples moved to use the 15.4 driver directly, bypassing Zephyr
networking — but that's an *sample* choice; for applications using `zsock_*` (us,
for MQTT), you keep `CONFIG_NETWORKING=y` + `CONFIG_NET_L2_OPENTHREAD=y`. No API
rename we use is named in any migration guide.

### Zephyr Settings subsystem

Used by `tinygs_config.cpp`:
- `settings_subsys_init`, `settings_register`, `settings_load_subtree`,
  `settings_save_one`, `settings_delete`, `settings_handler`

**Risk:** LOW for the API. But v3.0 made ZMS the default settings backend; to stay
on the NVS backend we'd add `CONFIG_SETTINGS_NVS=y` (or accept the ZMS default and
migrate the on-flash format, which loses saved Thread credentials / station config
on first boot). Strong recommendation: explicitly pin NVS to avoid surprise data
migration.

### USB device stack

Used by `main.cpp`:
- `usb_enable`, `usb_disable`, `cdc_acm_dte_rate_callback_set`, `uart_line_ctrl_get`
- MSC driver via `CONFIG_USB_DEVICE_STACK=y` + `CONFIG_USB_MASS_STORAGE=y`

**Risk:** MEDIUM. The v3.x line introduces the "USB device next" stack as the
recommended path. The legacy USB device stack still works but is on borrowed time
(deprecation horizon not yet set but publicly flagged). For v3.3 we can keep the
legacy stack; we should plan the "device next" migration as its own task (not
blocking). MSC + CDC ACM composite works on both.

### mbedTLS / TLS

No direct mbedTLS API calls — everything goes through Zephyr `zsock_*` with
`TLS_SEC_TAG_LIST` / `TLS_CREDENTIAL_CA_CERTIFICATE`. Kconfig surface: 40+ options
covering ciphersuites, RSA, ECDHE, session cache, heap size.

**Risk:** MEDIUM-HIGH. Our TLS handshake was hand-tuned to match
`mqtt.tinygs.com`'s `ECDHE-RSA-CHACHA20-POLY1305-SHA256` preference, and we explicitly
set `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` because the legacy RSA verify path was the
only thing that worked. Across v2.6 → v3.3, mbedTLS was bumped (3.5 → 3.6 lineage),
Oberon PSA went from 1.5.x→1.5.4, and default cipher enablement drifted. Expected
work: re-validate the handshake, possibly re-tune cipher suite config. Budget a day
for this alone.

### FS / FATFS

Used by `main.cpp` for USB MSC-backed config.json:
- `fs_mount`, `fs_unmount`, `fs_open`, `fs_read`, `fs_write`, `fs_truncate`,
  `fs_sync`, `fs_close`, `fs_file_t_init`, `fs_stat`, `fs_dirent`, `fs_mount_t`

**Risk:** LOW. FATFS in NCS is stable across versions; `FS_MOUNT_FLAG_NO_FORMAT`
semantics unchanged; the `CONFIG_FS_FATFS_*` kconfig surface is stable.

### Display

Used by `tinygs_display.cpp`:
- `display_blanking_on`, `display_blanking_off`, `display_write`,
  `DEVICE_DT_GET_OR_NULL(st7789v)`

**Risk:** LOW. ST7789V driver unchanged in structure. If we ever add the TFT_EN
runtime-gating (PLAN §4.3), the PM `RESUME` path may have been improved between
versions — worth checking upstream after we upgrade.

### Watchdog, k_work, MQTT client

Stable APIs across the gap; no action expected.

### Kconfig surface

142 `CONFIG_*` entries in our `prj.conf`. Full diff against v3.3.0 defaults is a
build-system task — any removed symbol produces a build warning; any renamed symbol
produces a silent kconfig default, which is the dangerous case. Plan: run a build
with v3.3.0, collect all kconfig-related warnings, remediate one by one.

### Partition layout

Not an API but a meta concern. PLAN.md §3.3 documents:
- Adafruit bootloader at 0x0F4000 (top)
- App at 0x026000
- FATFS at 0x0E2000, NVS at 0x0F2000

v3.3.0 deprecated the NCS Partition Manager in favour of Zephyr DTS partitioning.
Our layout is already DTS-driven (in `app.overlay`), so this is mostly a no-op —
but we need to confirm the DTS partition syntax hasn't changed.

---

## 3. What changes between v2.6 and v3.3 we have to address

In rough order of hands-on-keyboard effort:

### 3.1 Blocking (won't build / won't run without)

| Item | Source | Fix |
|---|---|---|
| ZMS default settings backend | v3.0 migration guide | Add `CONFIG_SETTINGS_NVS=y` explicitly to prj.conf; pin to NVS to keep on-flash format compatible with v2.6 data. |
| OpenThread samples bypass Zephyr networking by default | v3.1 migration guide | Not a migration per se, but confirm `CONFIG_NETWORKING=y` + `CONFIG_NET_L2_OPENTHREAD=y` stay set explicitly; we rely on them for MQTT/zsock. |
| Partition Manager deprecation | v3.3 migration guide | Confirm our DTS partition definitions still valid; no Partition Manager usage to remove (we already DTS-only). |
| mbedTLS kconfig drift | v3.0-v3.3 release notes | Full `prj.conf` kconfig audit; add a fresh `menuconfig` pass to resolve any removed/renamed options. Most critical subset: verify `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` still honoured and the legacy RSA path still buildable. |

### 3.2 Behavioral (will build, might behave differently)

| Item | Source | Validation |
|---|---|---|
| OT DNS client NAT64 fallback logic | commits `4c45ee8e0` + `d163dee2a` | Re-test end-to-end MQTT-over-NAT64 resolve; should be *better* not worse. |
| mbedTLS 3.5 → 3.6 cipher defaults | Zephyr bump | Capture the TLS handshake with Wireshark pre- and post-upgrade; confirm same ciphersuite negotiated or a compatible one. |
| USB device classic semantics | legacy but still supported | Re-test 1200-baud bootloader entry, MSC mount behaviour on macOS/Windows/Linux, CDC ACM console. |
| Thread joiner / DTLS commissioning | OT stack updates | Re-run `otJoinerStart` against HA SkyConnect BR; confirm PSKd still works. |
| Zephyr k_work_q internals | long list of fixes | Our SNTP + VBUS work items are simple enough to be insensitive. |

### 3.3 Optional cleanups enabled by the upgrade

- If v3.x Zephyr HTTP server is sufficient, drop the "hand-rolled HTTP/1.0 fallback"
  from the Phase 3 web UI plan.
- If USB device-next is stable, plan a follow-up migration off the legacy stack.
- If there's a nRF52840 driver improvement we've been patching around (none known
  today, but worth an end-of-upgrade grep), drop the workaround.

---

## 4. Recommended upgrade sequence

Phase-gated so a failure in one phase doesn't corrupt the working tree.

### Phase 0 — Baseline (t=0)

- Tag current working firmware as `v0.2` once the pre-flash stack
  (`5896f74` + `527e64d`) validates on hardware. This becomes the rollback point.
- Capture: full prj.conf, west.yml manifest revisions, sizes (flash/RAM), TLS
  handshake PCAP, lora_rx count over a known pass, MQTT reconnect timing.
- Snapshot the `ncs/` workspace (`west list > /tmp/west-baseline.txt`).

### Phase 1 — Toolchain & workspace update (1 day)

- `west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.0 ncs_new/`
- `west update` in fresh tree.
- Download Zephyr SDK version matching v3.3.0 (likely 0.17.x).
- Don't touch `src/` yet. Build our app against the new tree with `--board t114`.
- Expected: hundreds of kconfig warnings + possible build break. Collect them all.

### Phase 2 — Kconfig remediation (1-2 days)

- Walk every warning from Phase 1.
- Rename / remove / re-home deprecated options.
- Explicit pins: `CONFIG_SETTINGS_NVS=y`, `CONFIG_NETWORKING=y`,
  `CONFIG_NET_L2_OPENTHREAD=y` (may already be defaults but pin anyway).
- Iterate until clean build. Do NOT try to minimise prj.conf in this step —
  just make it build.
- Commit: `prj.conf: adjust for NCS v3.3.0 kconfig surface`.

### Phase 3 — API migration (1-2 days if no surprises)

- Fix any OpenThread / Zephyr API call site that no longer compiles.
- Likely minimal given our surface is stable public APIs.
- Commit per-area: `ot: adapt to v3.3 API`, `usb: adapt to v3.3 API`, etc.

### Phase 4 — TLS re-tune (1 day)

- Flash to device.
- Attempt MQTT-TLS connect to mqtt.tinygs.com.
- If handshake fails: capture PCAP, compare ciphersuite offer against baseline.
- Tune `CONFIG_MBEDTLS_*` kconfig to re-enable what's needed.
- Confirm reconnect cadence, TLS session resumption.

### Phase 5 — Thread re-validation (1 day)

- Re-commission to HA SkyConnect BR.
- Confirm joiner PSKd flow still works.
- Confirm DNS-over-NAT64 resolves mqtt.tinygs.com correctly — this is where we
  expect the upstream OT fixes to show as *improvements* rather than regressions.
- Run for 24 h, confirm no parent-loss power spikes, no spurious reboots.

### Phase 6 — Soak + tag (2-3 days)

- Full battery run on the deployed hardware.
- Compare average current against `v0.2` baseline. Should be equal or better;
  any regression > 1 mA is a blocker.
- Compare `lora_rx` count over equivalent pass geometry. Should be equal.
- Tag `v0.3` at the first commit that passes the soak.

### Rollback plan

If any phase fails and we can't diagnose quickly, revert to `v0.2`, flash the
existing build from the tag, and file the investigation as a separate task. We
never run a half-migrated tree on hardware for more than a day.

**Total realistic budget: 7–10 days of focused work.** Not something to slip in
alongside feature work.

---

## 5. Gating conditions (when to start)

Don't start the upgrade until all are true:

- Current pre-flash stack (`5896f74` + `527e64d`) flashed and validated on hardware.
- Phase 3 device web UI shipped (or explicitly deprioritised).
- Power run #3 has established a clean baseline on the post-hardening build, so
  regressions are measurable.
- A 2-week window with no other in-flight feature work.

Until all four are true: stay on v2.6.0.

---

## 6. What we are *not* doing

Explicitly out of scope for this upgrade:

- nRF54L, nRF54H20, nRF91, nRF70 support. We're nRF52840-only.
- Matter, BLE Mesh, Wi-Fi, cellular. We're Thread + LoRa + TLS-to-MQTT only.
- ZMS settings backend. We deliberately stay on NVS.
- PSA Crypto as default. We keep the legacy RSA path that matches mqtt.tinygs.com.
- USB device-next migration. Separate follow-up task after v3.3 is stable.
- Anything beyond the v3.3.0 tag. No chasing main-branch.

---

## 7. Open questions for the day-of-upgrade

- Does `mqtt.tinygs.com`'s ciphersuite preference still line up with what v3.3's
  mbedTLS enables by default? If not, which kconfig pin makes it work?
- Does v3.3's OpenThread have any new defaults for MTD child timeout or poll
  period that would affect our MQTT keepalive pinning strategy?
- Is there a Zephyr-side USB MSC change that affects auto-format behaviour on
  first-boot FATFS?
- Does the Zephyr v3.7 HTTP server change our Phase 3 budget estimate
  (PLAN §21 currently assumes v3.5 costs)?

These get answered when we do the work; don't speculate before Phase 0.
