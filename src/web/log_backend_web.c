/*
 * TinyGS nRF52 — Web UI Log Backend
 *
 * Second log backend (in addition to UART CDC + iot_log multicast).
 * Captures every formatted log line into a 4 KB ring buffer, tagged
 * with a monotonically-increasing sequence number, so the /cs HTTP
 * handler can do "give me everything since seq=N" short-poll queries.
 *
 * Layout in the ring (per-line):
 *   [4-byte seq | 4-byte epoch (UTC seconds) | 2-byte len | <len> bytes formatted text]
 *
 * The epoch is captured at queue time (time(NULL) — SNTP-synced after
 * boot). Read path formats it as "HH:MM:SS " and prepends to the line
 * so the dashboard console shows wall-clock timestamps. Lines queued
 * before SNTP sync get a "??:??:?? " prefix.
 *
 * Producer: Zephyr log subsystem (deferred mode, dedicated logging
 *           thread — char_out runs there).
 * Consumer: web_log_read_since() called from the HTTP server thread.
 *
 * Producer/consumer protected by a single k_spinlock — short critical
 * sections only (memcpy bytes into ring, advance pointers).
 *
 * If the ring fills, oldest lines are dropped to make room for new
 * ones (head-overwrites-tail). This matches how a tail -f would
 * behave on a fast-spewing log.
 */

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Real wall-clock epoch (seconds since 1970-01-01 UTC). Returns 0
 * before SNTP sync. Defined in main.cpp; libc time() returns uptime
 * on Zephyr without an explicit RTC driver, so we can't use that. */
extern int64_t get_utc_epoch(void);

/* Ring buffer size — sized for ~10 typical log lines plus the
 * 10-byte per-line header. Lost lines are silently overwritten
 * (head-overwrites-tail) when the buffer fills.
 * Trimmed 4096→2048 to reclaim 2 KB RAM; only reduces web-console
 * scroll-back depth (the /cs short-poll fetches incrementally by seq,
 * so a deeper client cursor than the ring holds just sees a gap). */
#define WEB_LOG_RING_SIZE 2048

/* Per-line header layout (little-endian everywhere):
 *   bytes 0..3   uint32_t seq      monotonic, never wraps in practice
 *   bytes 4..7   uint32_t epoch    seconds since 1970 UTC, 0 = pre-sync
 *   bytes 8..9   uint16_t len      formatted text length, no NUL
 * `WEB_LOG_HDR_SIZE` is computed below — never hard-code it. */
#define WEB_LOG_HDR_SEQ_OFF    0
#define WEB_LOG_HDR_EPOCH_OFF  4
#define WEB_LOG_HDR_LEN_OFF    8
#define WEB_LOG_HDR_SIZE       (WEB_LOG_HDR_LEN_OFF + sizeof(uint16_t))

/* Wall-clock prefix attached to every line on read: "HH:MM:SS " (9
 * chars including the trailing space). Derive the length from the
 * literal so a format change doesn't desync. Pre-SNTP entries get
 * a same-width placeholder so the textarea column stays aligned. */
#define WEB_LOG_TS_PREFIX_FMT     "%02d:%02d:%02d "
#define WEB_LOG_TS_PREFIX_UNKNOWN "??:??:?? "
#define WEB_LOG_TS_PREFIX_LEN     (sizeof(WEB_LOG_TS_PREFIX_UNKNOWN) - 1)

static uint8_t  ring[WEB_LOG_RING_SIZE];
static size_t   head;          /* write index */
static size_t   tail;          /* read-from index */
static size_t   used;          /* bytes currently in ring */
static uint32_t next_seq = 1;  /* next sequence to assign */
static struct k_spinlock ring_lock;

/* Format buffer for the current message (logging thread is single-
 * threaded; no lock needed for these statics). */
static char     msg_buf[256];
static size_t   msg_pos;
static uint8_t  log_buf[256];

static int char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < length && msg_pos < sizeof(msg_buf) - 1; i++) {
		if (data[i] != '\n' && data[i] != '\r') {
			msg_buf[msg_pos++] = data[i];
		}
	}
	return length;
}

LOG_OUTPUT_DEFINE(log_output_web, char_out, log_buf, sizeof(log_buf));

/* Write `n` bytes into the ring (head-relative), wrapping. Caller holds lock. */
static void ring_put(const void *src, size_t n)
{
	const uint8_t *p = (const uint8_t *)src;
	size_t first = WEB_LOG_RING_SIZE - head;
	if (first >= n) {
		memcpy(&ring[head], p, n);
	} else {
		memcpy(&ring[head], p, first);
		memcpy(&ring[0], p + first, n - first);
	}
	head = (head + n) % WEB_LOG_RING_SIZE;
	used += n;
}

/* Read `n` bytes from `pos` (relative to ring), wrapping. Does not advance state. */
static void ring_peek(size_t pos, void *dst, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	size_t first = WEB_LOG_RING_SIZE - pos;
	if (first >= n) {
		memcpy(d, &ring[pos], n);
	} else {
		memcpy(d, &ring[pos], first);
		memcpy(d + first, &ring[0], n - first);
	}
}

/* Drop the oldest entry to make room. Caller holds lock.
 * Returns false if the ring is empty. */
static bool drop_oldest(void)
{
	if (used < WEB_LOG_HDR_SIZE) {
		return false;
	}
	uint8_t hdr[WEB_LOG_HDR_SIZE];
	ring_peek(tail, hdr, WEB_LOG_HDR_SIZE);
	uint16_t len = ((uint16_t)hdr[WEB_LOG_HDR_LEN_OFF]     << 8) |
		       ((uint16_t)hdr[WEB_LOG_HDR_LEN_OFF + 1]);
	size_t entry = WEB_LOG_HDR_SIZE + len;
	if (entry > used) {
		/* Corrupt — flush everything to recover. */
		head = tail = 0;
		used = 0;
		return false;
	}
	tail = (tail + entry) % WEB_LOG_RING_SIZE;
	used -= entry;
	return true;
}

static void enqueue_line(const char *text, size_t len)
{
	if (len > 0xFFFF) {
		len = 0xFFFF;
	}
	size_t need = WEB_LOG_HDR_SIZE + len;
	if (need > WEB_LOG_RING_SIZE) {
		return;  /* line too big for the ring; drop */
	}

	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	/* Make room if needed. */
	while (used + need > WEB_LOG_RING_SIZE) {
		if (!drop_oldest()) {
			break;
		}
	}

	uint32_t seq = next_seq++;
	uint32_t epoch = (uint32_t)get_utc_epoch();  /* 0 if SNTP not yet synced */
	uint8_t hdr[WEB_LOG_HDR_SIZE];
	hdr[WEB_LOG_HDR_SEQ_OFF + 0] = (uint8_t)(seq         & 0xFF);
	hdr[WEB_LOG_HDR_SEQ_OFF + 1] = (uint8_t)((seq >> 8)  & 0xFF);
	hdr[WEB_LOG_HDR_SEQ_OFF + 2] = (uint8_t)((seq >> 16) & 0xFF);
	hdr[WEB_LOG_HDR_SEQ_OFF + 3] = (uint8_t)((seq >> 24) & 0xFF);
	hdr[WEB_LOG_HDR_EPOCH_OFF + 0] = (uint8_t)(epoch         & 0xFF);
	hdr[WEB_LOG_HDR_EPOCH_OFF + 1] = (uint8_t)((epoch >> 8)  & 0xFF);
	hdr[WEB_LOG_HDR_EPOCH_OFF + 2] = (uint8_t)((epoch >> 16) & 0xFF);
	hdr[WEB_LOG_HDR_EPOCH_OFF + 3] = (uint8_t)((epoch >> 24) & 0xFF);
	hdr[WEB_LOG_HDR_LEN_OFF + 0] = (uint8_t)((len >> 8) & 0xFF);
	hdr[WEB_LOG_HDR_LEN_OFF + 1] = (uint8_t)(len        & 0xFF);
	ring_put(hdr, WEB_LOG_HDR_SIZE);
	ring_put(text, len);

	k_spin_unlock(&ring_lock, key);
}

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	msg_pos = 0;
	uint32_t flags = 0;
	log_format_func_t fmt = log_format_func_t_get(LOG_OUTPUT_TEXT);
	if (fmt == NULL) {
		return;
	}
	fmt(&log_output_web, &msg->log, flags);
	if (msg_pos == 0) {
		return;
	}
	enqueue_line(msg_buf, msg_pos);
}

static void panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static const struct log_backend_api log_backend_web_api = {
	.process = process,
	.panic = panic,
};

/* autostart=true: start emitting as soon as the log subsystem comes up.
 * Unlike iot_log, the ring is local memory and always available, so no
 * gating is needed. */
LOG_BACKEND_DEFINE(log_backend_web, log_backend_web_api, true);

/* ===== Public API consumed by web_ui.cpp's /cs handler ===== */

int web_log_read_since(uint32_t since_seq, char *out, int cap, uint32_t *out_seq)
{
	if (cap <= 0 || out == NULL) {
		return 0;
	}

	int written = 0;
	uint32_t high = since_seq;

	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	size_t pos = tail;
	size_t remaining = used;

	while (remaining >= WEB_LOG_HDR_SIZE) {
		uint8_t hdr[WEB_LOG_HDR_SIZE];
		ring_peek(pos, hdr, WEB_LOG_HDR_SIZE);
		uint32_t seq = (uint32_t)hdr[WEB_LOG_HDR_SEQ_OFF + 0] |
			       ((uint32_t)hdr[WEB_LOG_HDR_SEQ_OFF + 1] << 8) |
			       ((uint32_t)hdr[WEB_LOG_HDR_SEQ_OFF + 2] << 16) |
			       ((uint32_t)hdr[WEB_LOG_HDR_SEQ_OFF + 3] << 24);
		uint32_t epoch = (uint32_t)hdr[WEB_LOG_HDR_EPOCH_OFF + 0] |
				 ((uint32_t)hdr[WEB_LOG_HDR_EPOCH_OFF + 1] << 8) |
				 ((uint32_t)hdr[WEB_LOG_HDR_EPOCH_OFF + 2] << 16) |
				 ((uint32_t)hdr[WEB_LOG_HDR_EPOCH_OFF + 3] << 24);
		uint16_t len = ((uint16_t)hdr[WEB_LOG_HDR_LEN_OFF]     << 8) |
			       ((uint16_t)hdr[WEB_LOG_HDR_LEN_OFF + 1]);
		size_t entry = WEB_LOG_HDR_SIZE + len;
		if (entry > remaining) {
			break;
		}

		if (seq > since_seq) {
			/* Format the wall-clock timestamp prefix. Pre-SNTP
			 * entries (epoch == 0) get a same-width placeholder
			 * so the textarea column stays aligned. */
			char ts[WEB_LOG_TS_PREFIX_LEN + 1];
			if (epoch > 0) {
				time_t t = (time_t)epoch;
				struct tm tm_local;
				localtime_r(&t, &tm_local);
				snprintf(ts, sizeof(ts), WEB_LOG_TS_PREFIX_FMT,
					 tm_local.tm_hour, tm_local.tm_min,
					 tm_local.tm_sec);
			} else {
				memcpy(ts, WEB_LOG_TS_PREFIX_UNKNOWN,
				       WEB_LOG_TS_PREFIX_LEN);
				ts[WEB_LOG_TS_PREFIX_LEN] = '\0';
			}
			/* Need: ts + line text + '\n'. Skip if it won't fit. */
			const int needed = (int)WEB_LOG_TS_PREFIX_LEN
					   + (int)len + 1 /* '\n' */;
			if (written + needed > cap) {
				break;
			}
			memcpy(out + written, ts, WEB_LOG_TS_PREFIX_LEN);
			written += (int)WEB_LOG_TS_PREFIX_LEN;
			ring_peek((pos + WEB_LOG_HDR_SIZE) % WEB_LOG_RING_SIZE,
				  out + written, len);
			written += (int)len;
			out[written++] = '\n';
			high = seq;
		}

		pos = (pos + entry) % WEB_LOG_RING_SIZE;
		remaining -= entry;
	}

	k_spin_unlock(&ring_lock, key);

	if (out_seq) {
		*out_seq = high;
	}
	return written;
}

uint32_t web_log_head_seq(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t h = next_seq - 1;
	k_spin_unlock(&ring_lock, key);
	return h;
}
