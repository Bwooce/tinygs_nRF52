# TinyGS nRF52 — Technical Audit & Improvement Plan

> Status: analysis deliverable. Findings are grounded in specific files/lines.
> Sub-agent claims were verified against source; three false positives were
> discarded (noted at the end). One config-reclaim subset (Wins 1–3) has been
> applied to `prj.conf` / `log_backend_web.c` and must be build+soak verified
> before merge.

## Executive Summary

**Overall health grade: B−** (calibrated to a solo experimental PoC, weighted
by its stated goal of running unattended 24/7 against a production MQTT server).

Impressive single-author firmware: a clean-room reimplementation of ESP32
TinyGS on nRF52840 + Zephyr + OpenThread (~3 weeks, 64 commits, one author).
Observability and memory engineering are well above PoC norms — layered
watchdogs, `__noinit` crash forensics with `addr2line` hints, per-subsystem
RAM/stack/net-buf instrumentation, and a heavily-reasoned `prj.conf`. It ships
real unit tests (283 assertions / 99 cases for the JSON parser). What drags the
grade down: TLS server-certificate verification is disabled, the codebase is
dominated by a 3,792-line `main.cpp` god file, there is no CI running the good
tests that already exist, and output encoding (HTML/JSON/CSV) is systematically
missing.

**Top 3 risks:** (1) `TLS_PEER_VERIFY_NONE` makes the MQTT-TLS link MITM-able
and renders the bundled CA cert decorative; (2) `main.cpp` concentration + a
giant `switch` in `mqtt_evt_handler` makes every change high-risk and
untestable; (3) existing tests are not CI-gated, so regressions land silently.

**Top 3 opportunities:** (1) flip on peer verification — one line, closes the
headline gap, ~0 RAM cost; (2) a minimal CI that builds + runs the two ztest
suites; (3) extract the command handlers from `main.cpp` into `tinygs_protocol`
so the command surface becomes testable.

## Repo Map

LoRa satellite ground station for the Heltec Mesh Node T114 (nRF52840 + SX1262),
replacing ESP32/WiFi/Arduino with nRF52/OpenThread/Zephyr; MQTT-TLS to
`mqtt.tinygs.com` over a Thread mesh via NAT64. Maturity: experimental PoC,
explicitly "not for production" (README:19-20).

`main()` (main.cpp:3228) runs a 5-state machine: `WAIT_THREAD → DNS_RESOLVE →
MQTT_CONNECT → MQTT_CONNECTED → ERROR`. Inbound server commands arrive via
`mqtt_evt_handler` (main.cpp:1262); LoRa RX is interrupt-latched and drained in
`lora_check_rx` (main.cpp:3025). Side workqueues handle WDT feeding, USB VBUS,
SNTP resync, epoch persistence.

| Path | Role |
|------|------|
| `src/main.cpp` (3,792 L) | God file: state machine, MQTT command handler, radio init, watchdog, USB/FATFS, SNTP, Doppler, crash diagnostics |
| `src/tinygs_protocol.cpp` (645 L) | MQTT payload builders + `set_pos` |
| `src/tinygs_json.cpp` (350 L) | begine (ArduinoJson) + hand-rolled parsers + JSON escape |
| `src/tinygs_config.cpp` (235 L) | Zephyr Settings/NVS persistence |
| `src/web/web_ui.cpp` (1,299 L) | HTTP/1.1 server on `[::]:80` |
| `src/web/log_backend_web.c` | Ring-buffer log backend for the web console |
| `tests/{json_parser,p13_propagator}/` | ztest suites (not wired to CI) |

## Audit Report

### Architecture & Design
- **[High]** `main.cpp` god file (3,792 L). `mqtt_evt_handler` (1262-1929) is
  ~670 L; the `begine`/`batch_conf` branch alone (1369-1623) is ~250 L running
  JSON parse + radio reconfig + TLE decode + filter parse in the MQTT callback.
  Untestable; every edit risks the state machine.
- **[Medium]** Cross-TU coupling via `extern` globals; `device_client_id`
  redeclared inline (1276, 1725, 2316); `radio` pointer `extern`'d with
  duplicated `#if DT_NODE_HAS_COMPAT` blocks. `tinygs_radio` mutex-guarded in
  some readers but not the protocol payload builders (protocol.cpp:332,356-366,
  442-521) — torn reads possible, low impact.
- **[Low]** Two config-ingest paths (USB `config.json` diff, main.cpp:2367-2542,
  vs MQTT handlers) with different validation.

### Code Quality
- **[Medium]** Manual JSON parsers accept a `len` they then ignore, scanning with
  NUL-terminated funcs past `len` (json.cpp:160,220,227,247-281). Safe today only
  because the live caller NUL-terminates `rx_payload` (main.cpp:1331).
- **[Low]** Dead/duplicated code: `test_ZephyrHal.cpp` linked but uncalled
  (CMakeLists.txt:32); `led_set()` is a no-op (744-749); legacy
  `begin_lora`/`begin_fsk` array parsers duplicate an idiom 4×.
- **[Low]** `ret |=` on negative errnos (config.cpp:193-195,228-234) loses which
  save failed.
- **[Strength]** snprintf truncation consistently checked/logged; no unbounded
  loops found.

### Security
- **[High]** **TLS server cert verification disabled.** `peer_verify =
  TLS_PEER_VERIFY_NONE` (main.cpp:2013), set right after registering the CA cert
  (1998-2008) and SNI hostname (2017). The MQTT-TLS session authenticates
  nothing — any on-path host can MITM and inject server commands (incl.
  `reset`, `update`, retune, `set_name`→reboot). The bundled CA cert is dead
  weight.
- **[Medium]** Missing output encoding (systemic):
  - HTML: `cfg_station`/`cfg_mqtt_user` into `value='%s'` on the auth-gated
    `/config` form (web_ui.cpp:623/635, 629/639) → stored XSS. Both write and
    view paths require admin auth, so effectively authenticated/self-XSS; also
    breaks the form for legitimate names containing `'`.
  - JSON: `tinygs_radio.satellite` into welcome/RX/status unescaped
    (protocol.cpp:164,358,402,473,511) — inconsistent, since `modem_conf` /
    `boardTemplate` *are* escaped (95,129).
  - CSV: satellite/modem fields in `/wm` (spreadsheet formula-injection if a
    name starts with `=`).
- **[Medium]** Web UI is HTTP + Basic auth: admin password traverses the network
  base64 (cleartext) on each authenticated request; no CSRF token on POST.
- **[Low/Med]** Default admin password `"tinygs"` shipped in source
  (config.cpp:18), not forced to rotate.
- **[Low]** No range validation on MQTT `set_pos_prm` (protocol.cpp:543-585) vs
  the file path's clamps (main.cpp:2442-2453).
- **[Strength]** No broker creds in repo (`mqtt_credentials.h` gitignored);
  Basic-auth compare is length-checked before `memcmp` (web_ui.cpp:113); strict
  allow-list `strcmp` dispatch (no shell); FATFS boot-sector validated before
  mount (2291-2307).

### Testing
- **[Strength]** JSON parser well tested (283 `zassert` / 99 `ZTEST`).
- **[High]** No CI of any kind — tests never run automatically.
- **[Medium]** Core command/TLS logic untestable until the handler is extracted.

### Performance
Healthy. Mid-packet retune correctly deferred (2858-2863); ext-flash DPD; 100 ms
loop with interrupt-latched RX. Minor: `read_vbat_mv()` does a full ADC setup +
2 ms settle on every call incl. inside the 5-min STATUS log — negligible.

### Dependencies
- **[Low]** RadioLib submodule is a personal fork (`Bwooce/RadioLib.git` branch
  `tinygs`) carrying private calibration — bus-factor consideration. Licensing
  handled carefully (MIT app, clean-room vs GPL ESP32).

### DevEx & Operations
- **[Medium]** No CI / linter / formatter. `build.sh` hard-codes core count and
  assumes local `./ncs` + `./.venv`.
- **[Strength]** `flash.sh` UF2 partition-safety preflight; rich self-reported
  STATUS; self-diagnosing crashes.

### Documentation
- **[Strength]** `prj.conf` / `AGENTS.md` / `PLAN.md` carry the *why* for nearly
  every non-obvious decision, incl. the bricked-bootloader post-mortem.
- **[Medium]** Stale docs: README NCS version (v3.5.99 vs v3.3.0 elsewhere);
  README "Web UI not implemented" is false; prj.conf frames Basic auth as future
  "Phase 4" though it's implemented.

## Upstream beta comparison (tinygs/tinyGS @ beta, 2026-04-08)

The port is **feature-current** with upstream beta:
- Command set matches exactly (MQTT_Client.h:148-171), plus our extra `remoteTune`.
- FSK decoder chain matches: `nrz2nrzi`, `descram1712` (x¹⁷+x¹²),
  `remove_bit_stuffing`, `pn9` all present in `src/bitcode.c`.
- Telemetry struct (`tinygs_radio_state`) aligns with upstream `ModemInfo` /
  `PacketInfo` / `Tle`.

Upstream-only code is ESP32-specific and not portable: Improv WiFi provisioning,
ArduinoOTA/HTTPS OTA, AXP power management, IotWebConf2/pubsubclient.

Minor watch-items: cross-check `enc==10` (NRZ-on-127x / whitening-on-126x) and
`whitening_seed=0x01E1` handling. Track (don't merge) the WIP upstream branches
`refactor/connection-layer` and `feat/axp_hardware_id` (2026-05-25).

## Improvement Strategy

1. **Enforce both network trust boundaries.** Verify the broker cert; encode all
   output by context; treat the Thread mesh as semi-hostile.
2. **De-concentrate logic to enable testing.** Command handlers as pure-ish
   functions in `tinygs_protocol`, callable from ztest.
3. **Gate the good tests.** CI builds firmware + runs both ztest suites on push.
4. **Fix doc drift.**

**Not recommending:** MCUboot/OTA rework (hardware-gated Phase 3); migrating off
the RadioLib fork; enterprise observability/secrets-management; rewriting the
display/worldmap renderer; **adding web-UI HTTPS (see T5).**

**Done =** zero Critical/High security findings; CI green running both suites;
`main.cpp` < ~2,500 L with command dispatch relocated + unit-tested; README
matches code.

## Task Plan

### Quick wins (S effort)
- **QW1** Enable TLS peer verification (main.cpp:2013).
- **QW2** HTML-escape `cfg_station`/`cfg_mqtt_user` (web_ui.cpp:635,639).
- **QW3** Escape `satellite` in JSON builders (protocol.cpp:358,402,473,511).
- **QW4** Fix stale README (NCS version, web UI, Phase-4 auth).

### Tasks
| ID | Title | Files | Effort | Risk | Deps |
|----|-------|-------|--------|------|------|
| T1 | Broker cert verify (`PEER_VERIFY_REQUIRED`) — ~0 RAM cost | main.cpp:1998-2018 | S | Med | — |
| T2 | Context output-encoding pass (HTML/JSON/CSV) | web_ui.cpp, protocol.cpp, json.cpp | M | Low | — |
| T3 | CI: build + run ztest (twister, native_sim) | new `.github/workflows/` | M | Low | — |
| T4 | Extract command dispatch into `tinygs_protocol` | main.cpp:1344-1908 | L | Med | T3 |
| T5 | **Keep web UI HTTP** (revised — see below) + CSRF/Origin check | web_ui.cpp | S/M | Low | — |
| T6 | Force admin-pw change from default / warn until changed | config.cpp:18, web_ui | S | Low | — |
| T7 | Validate `set_pos_prm` ranges | protocol.cpp:543-585 | S | Low | — |
| T8 | Harden manual parsers to honour `len` | json.cpp | M | Low | T3 |
| T9 | Remove dead code (test_ZephyrHal, led_set no-op, legacy parsers) | CMakeLists.txt:32, main.cpp | S | Low | — |
| T10 | Add `.clang-format` + format check in CI | repo root | S | Low | T3 |
| T11 | **Memory budget reclaim** (Wins 1–6 below) | prj.conf, web_ui.cpp, log_backend_web.c | S–M | Low–Med | — |

### Milestones
- **M0 (safety net):** T3, T10.
- **M1 (critical/correctness):** T1, T2, T7.
- **M2 (high-leverage):** T4, T5, T8, T11.
- **M3 (polish):** T6, T9, QW4.

### Implementation sketches (top 3)
**T1** Flip `TLS_PEER_VERIFY_NONE`→`REQUIRED` (2013); keep CA registration +
SNI. Confirm `tinygs_ca_cert.h` carries the full chain (intermediate incl.);
SNTP already runs pre-connect (3497) so validity-window checks have a clock —
but handle `time=0` boots by retrying after SNTP rather than disabling verify.

**T4** Define `tinygs_handle_command(cmd, payload, len, client)` in
tinygs_protocol.cpp; move the `strcmp` ladder (1349-1908) in verbatim under CI,
then unit-test pure decoders. Preserve the reboot/TX latch idiom
(`web_ui_pop_*`); no blocking calls in the MQTT callback.

**T3** GitHub Actions on the NCS toolchain container; `west build` for
`heltec_mesh_node_t114/nrf52840` (build-only) + `twister` on both suites
(native_sim). Pin NCS v3.3.0; needs submodules + `BOARD_ROOT`.

## Memory (256 KB nRF52840)

### Why NOT web-UI HTTPS
Adding server-side TLS means a **second** mbedTLS SSL context (another
`SSL_IN/OUT_CONTENT_LEN` pair, ~10 KB+) plus a concurrent handshake heap spike.
The single TLS heap (32 KB) already peaks ~21.7 KB (54%, prj.conf:410-414); a
second overlapping handshake won't fit. **Keep the web UI HTTP-only** and rely on
802.15.4 L2 AES-CCM (mesh encryption) + Basic auth; the only cleartext exposure
is the BR→LAN segment — document that boundary instead of spending the RAM.

Distinction: **T1 (broker cert verify) adds ~0 steady-state RAM** — the client
TLS context already exists; verification is CPU + a transient bump in the
existing handshake heap (which has ~10 KB headroom). The memory worry applies
only to *new* TLS.

### Establishing the baseline (honest caveat)
No NCS toolchain in the audit container — no exact byte baseline from a build.
Two authoritative sources:
1. The firmware already prints a live baseline every 5 min — the `STATUS:` line
   (main.cpp:3732): `heap`, `mtls(peak)`, `stack`, `http_stack`, `nbuf`/`pkt`.
   Capture one after an MQTT reconnect + `/dashboard` fan-out + a LoRa RX.
2. `west build -t ram_report` (+ `rom_report`) for the static breakdown.

Configured static budget (author-measured peaks in comments):

| Consumer | Configured | Peak | Ref |
|---|---|---|---|
| mbedTLS heap | 32 KB | 21.7 KB (54%) | prj.conf:414 |
| mbedTLS SSL IN / OUT | 8 KB / 2 KB | — | prj.conf:437,446 |
| OT message buffers | 80 × ~128 B ≈ 10 KB | — | prj.conf:364 |
| net_buf TX / RX | 64 / 32 × ~152 B ≈ 15 KB | TX 47/64 | prj.conf:304-312 |
| Main stack | 12 KB | ~6 KB | prj.conf:56 |
| Web UI static bodies | 6.4 KB | resident | web_ui.cpp:218/295/517/714/944 |
| HTTP client buffers | 4 × 1 KB | — | MAX_CLIENTS |
| Log buffer + web ring | 4 + 4 KB | <4 KB | prj.conf:121 |
| Unified heap pool | 4 KB | ~1.7 KB | prj.conf:70 |
| Protocol static buffers | ~4 KB | — | payload_buf 2048 + esc 1024 + … |

AGENTS.md §3's ~162 KB / 62% baseline predates the web UI + ext-flash, so live
RAM is now higher — confirm via the STATUS line.

### Reclaim opportunities (no new TLS)
**Applied (trimmed subset, ~6 KB) — pending build+soak verify:**
1. `HTTP_SERVER_MAX_CLIENTS` 4→2 (~2 KB). Left `ZVFS_OPEN_MAX`/`NET_MAX_CONN`
   alone (shared with broker socket + DNS; eventfd `-ENOMEM` risk).
2. `LOG_BUFFER_SIZE` 4096→2048 + `WEB_LOG_RING_SIZE` 4096→2048 (~4 KB). Watch for
   new dropped-log lines under a reconnect storm.

**Deferred (proposed but not landed):**
- `HEAP_MEM_POOL_SIZE` 4096→3072 (~1 KB). Live STATUS from a real run shows
  `peak=2888` B, contradicting the audit's "< 2.5 KB" assertion. 3072 would
  leave only ~184 B headroom — a single allocation burst could OOM. Keep at
  4096 until peak demonstrably stays under ~2.5 KB across reconnects + RX.

**Optional (medium risk, separate change):**
4. `MBEDTLS_HEAP_SIZE` 32768→28672 (~4 KB) — peak 21.7 KB, verify never >24 KB
   across a reconnect *overlap*.
5. Unify the five web-UI `static char body[]` buffers (6.4 KB) into one shared
   3072 scratch (~3 KB) — safe only because handlers dispatch sequentially.

**Avoid (last resort):** dropping `SSL_IN_CONTENT_LEN` below 8192 — needs
server-side fragmentation, risks breaking the broker handshake (AGENTS.md:46).

### Verification gate (before landing any reclaim)
1. `./build.sh` then `west build -t ram_report` — record the delta.
2. Soak: force MQTT reconnect, load `/dashboard`, trigger a LoRa RX; capture one
   `STATUS:` line.
3. Confirm `heap`, `mtls(peak)`, `nbuf`/`pkt` retain margin; no new dropped-log
   lines.

## Open Questions
1. Web-UI threat model: is the Thread mesh / LAN trusted? (Drives T5 depth.)
2. Does `tinygs_ca_cert.h` carry the full chain, and is pinning to that CA OK if
   the broker rotates issuers? (Drives T1 effort.)
3. Hold the project to PoC or always-on standards? (Audit graded security to the
   latter.)
4. Is upstreaming RadioLib's `zephyr-hal` on the roadmap, or is the `tinygs` fork
   permanent? (Bus-factor.)
5. Are legacy `begin_lora`/`begin_fsk` array commands still server-emitted, or
   safe to delete in T9?

## Sub-agent false positives discarded (verified against source)
- "Format-string attack via satellite name" (protocol.cpp:358) — it is a `%s`
  *argument*, not the format string. Not a vuln.
- "Critical off-by-one in `tinygs_json_escape`" — `n+2 < dstlen` is safe (merely
  conservative); never overflows.
- "Auth prefix-bypass in `basic_auth_ok`" — web_ui.cpp:113 checks length equality
  before `memcmp`. Safe.
