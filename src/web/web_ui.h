/*
 * TinyGS nRF52 — Device Web UI (Phase 1+2)
 *
 * In-tree Zephyr HTTP server on port 80 over Thread mesh-local address.
 * Endpoints:
 *   GET /         — minimal status text
 *   GET /restart  — schedules a cold reboot
 *   GET /cs?c2=N  — returns log lines with seq > N (dedicated log backend ring)
 *
 * Reachable from any host with a route to the Thread mesh-local /64 prefix.
 * No TLS today (plain HTTP); upgrade path is in PLAN.md §21.5.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP server. Call after Thread is attached.
 * Idempotent — repeat calls are no-ops.
 */
int web_ui_start(void);

/* Log backend ring API — implemented in log_backend_web.c.
 * `iot_log_send_raw`-style entry point for the /cs handler. */

/**
 * Read up to `cap` bytes of formatted log lines emitted *after* the
 * given sequence. Writes the new high-watermark sequence into *out_seq.
 *
 * Returns bytes written (may be zero if no new lines). The output is
 * a stream of '\n'-separated lines, no trailing newline guarantee.
 * The output is also truncated to whole-line boundaries; partial lines
 * are deferred to the next call.
 */
int web_log_read_since(uint32_t since_seq, char *out, int cap, uint32_t *out_seq);

/**
 * Current head sequence (last emitted line). 0 at boot.
 */
uint32_t web_log_head_seq(void);

#ifdef __cplusplus
}
#endif
