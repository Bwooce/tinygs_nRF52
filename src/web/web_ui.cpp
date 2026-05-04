/*
 * TinyGS nRF52 — Device Web UI (Phase 1+2)
 *
 * In-tree Zephyr HTTP server on port 80.
 *
 * Listens on the unspecified address (`::`) so it accepts connections on
 * any interface — Thread mesh-local, link-local, USB CDC etc. The expected
 * reachability path is from a same-LAN host with a route to the Thread
 * mesh-local /64 prefix (configured at the BR side), or from a phone joined
 * directly to the Thread mesh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/logging/log.h>

#include <time.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>

#include "web_ui.h"
#include "tinygs_protocol.h"   /* TINYGS_VERSION, tinygs_radio */
#include "tinygs_config.h"     /* cfg_station[] */

/* MQTT connection state, exposed by main.cpp via a small accessor so we
 * don't need to extern the file-local `enum app_state` (declarations would
 * have to match exactly across TUs and would silently break on reordering). */
extern "C" bool tinygs_mqtt_is_connected(void);

/* Provided by Zephyr OpenThread integration. Forward-declared so we don't
 * have to drag in zephyr/net/openthread.h (deprecated wrappers). */
extern "C" struct otInstance *openthread_get_default_instance(void);

LOG_MODULE_REGISTER(web_ui, LOG_LEVEL_INF);

/* ===== / handler — minimal status ===== */
static int root_handler(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx,
			void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static char body[256];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	int n = snprintf(body, sizeof(body),
			 "TinyGS nRF52\n"
			 "station: %s\n"
			 "version: %u\n"
			 "uptime_ms: %lld\n"
			 "log_seq: %u\n",
			 cfg_station[0] ? cfg_station : "tinygs",
			 (unsigned)TINYGS_VERSION,
			 (long long)k_uptime_get(),
			 web_log_head_seq());
	if (n < 0) {
		n = 0;
	}
	if (n >= (int)sizeof(body)) {
		n = sizeof(body) - 1;
	}

	response_ctx->body = (uint8_t *)body;
	response_ctx->body_len = (size_t)n;
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic root_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/plain",
	},
	.cb = root_handler,
	.user_data = NULL,
};

/* ===== /restart handler — schedules a cold reboot ===== */
static struct k_work_delayable restart_work;
static void restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("Reboot requested via /restart");
	sys_reboot(SYS_REBOOT_COLD);
}

static int restart_handler(struct http_client_ctx *client,
			   enum http_transaction_status status,
			   const struct http_request_ctx *request_ctx,
			   struct http_response_ctx *response_ctx,
			   void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static const char body[] = "Rebooting in 2 seconds.\n";

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	/* Defer the reboot so the response actually flushes to the client. */
	k_work_reschedule(&restart_work, K_MSEC(2000));

	response_ctx->body = (uint8_t *)body;
	response_ctx->body_len = sizeof(body) - 1;
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic restart_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/plain",
	},
	.cb = restart_handler,
	.user_data = NULL,
};

/* ===== /cs handler — console short-poll =====
 *
 * Query string: ?c2=<seq> (decimal sequence; 0 to get whatever is in the
 * ring right now). Response: lines newer than seq, separated by '\n', plus
 * a leading "seq: <new-high-watermark>\n" header so the client can update
 * its cursor.
 */
static int parse_seq_from_query(const char *url_buf, size_t url_len, uint32_t *out)
{
	/* Locate "c2=" and parse decimal until '&' or end. */
	if (url_buf == NULL || url_len == 0) {
		return -1;
	}
	for (size_t i = 0; i + 3 < url_len; i++) {
		if (url_buf[i] == 'c' && url_buf[i+1] == '2' && url_buf[i+2] == '=') {
			uint32_t v = 0;
			size_t j = i + 3;
			while (j < url_len && url_buf[j] >= '0' && url_buf[j] <= '9') {
				v = v * 10 + (uint32_t)(url_buf[j] - '0');
				j++;
			}
			*out = v;
			return 0;
		}
	}
	return -1;
}

static int cs_handler(struct http_client_ctx *client,
		      enum http_transaction_status status,
		      const struct http_request_ctx *request_ctx,
		      struct http_response_ctx *response_ctx,
		      void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	/* Cap response so it fits in HTTP_SERVER_CLIENT_BUFFER_SIZE (1024)
	 * after the response headers eat their share. The handler returns
	 * partial — the client uses the seq cursor to ask for more. */
	static char body[800];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	uint32_t since = 0;
	(void)parse_seq_from_query((const char *)client->url_buffer,
				   sizeof(client->url_buffer),
				   &since);

	uint32_t high = since;

	/* Reserve a fixed 16-byte prefix for the header. This avoids the
	 * snprintf NUL terminator clobbering the first byte of the log
	 * content when header and content share one buffer. */
	enum { HDR_RESERVE = 16 };
	int wrote = web_log_read_since(since,
				       body + HDR_RESERVE,
				       (int)sizeof(body) - HDR_RESERVE,
				       &high);

	/* Build the header into a small scratch, then memcpy (no NUL). */
	char hdr[HDR_RESERVE + 1];
	int hdr_n = snprintf(hdr, sizeof(hdr), "seq: %u\n", high);
	if (hdr_n < 0 || hdr_n > HDR_RESERVE) {
		hdr_n = 0;
	}
	/* Slide so the header sits flush against the content. */
	int hdr_off = HDR_RESERVE - hdr_n;
	memcpy(body + hdr_off, hdr, (size_t)hdr_n);

	response_ctx->body = (uint8_t *)(body + hdr_off);
	response_ctx->body_len = (size_t)(hdr_n + wrote);
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic cs_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/plain",
	},
	.cb = cs_handler,
	.user_data = NULL,
};

/* ===== /wm handler — worldmap + status CSV =====
 *
 * Same field order as the ESP32 dashboard's /wm endpoint so the JS that
 * already exists could parse our response without modification:
 *
 *   cx,cy,                                    sat pixel position
 *   modem_mode,frequency,freq_offset,         modem
 *   sf|bitrate,cr|freq_dev,bw,
 *   station_name,version,                     ground station
 *   mqtt_status,parent_rssi,radio_status,last_rssi,
 *   satellite,                                 sat info
 *   sat_lat/lon,sat_az/el,doppler_freq,
 *   utc_time,local_time,                      time
 *   last_pkt_time,last_pkt_rssi,last_pkt_snr,last_pkt_ferr,crc_state
 *
 * Read-side concurrency: snapshot under tinygs_radio_mutex so a multi-
 * field begine/batch_conf update can't tear across our memcpy. Lock is
 * held for ~50 µs (one cache-line copy); main-thread writers see at
 * worst a microsecond stall.
 */
static int wm_handler(struct http_client_ctx *client,
		      enum http_transaction_status status,
		      const struct http_request_ctx *request_ctx,
		      struct http_response_ctx *response_ctx,
		      void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static char body[800];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	/* Atomic snapshot under lock — see PLAN.md §21.9. */
	struct tinygs_radio_state snap;
	k_mutex_lock(&tinygs_radio_mutex, K_FOREVER);
	snap = tinygs_radio;
	k_mutex_unlock(&tinygs_radio_mutex);

	bool mqtt_up = tinygs_mqtt_is_connected();

	/* Sat pixel-coords match ESP32 mapping (offset by 3 for the SVG marker). */
	int cx = (int)(snap.sat_pos_x * 2.0f + 3.0f);
	int cy = (int)(snap.sat_pos_y * 2.0f + 3.0f);

	int n = 0;
	n += snprintf(body + n, sizeof(body) - n, "%d,%d,", cx, cy);

	n += snprintf(body + n, sizeof(body) - n, "%s,%.4f,%.0f,",
		      snap.modem_mode[0] ? snap.modem_mode : "-",
		      (double)snap.frequency,
		      (double)snap.freq_offset);

	if (strcmp(snap.modem_mode, "LoRa") == 0) {
		n += snprintf(body + n, sizeof(body) - n, "%d,%d,",
			      snap.sf, snap.cr);
	} else {
		n += snprintf(body + n, sizeof(body) - n, "%.1f,%.0f,",
			      (double)snap.bitrate, (double)snap.freq_dev);
	}
	n += snprintf(body + n, sizeof(body) - n, "%.1f,",
		      (double)snap.bw);

	n += snprintf(body + n, sizeof(body) - n, "%s,%u,",
		      cfg_station[0] ? cfg_station : "tinygs",
		      (unsigned)TINYGS_VERSION);

	n += snprintf(body + n, sizeof(body) - n, "%s,",
		      mqtt_up ? "<span class='G'>CONNECTED</span>"
			      : "<span class='R'>NOT CONNECTED</span>");

	/* Parent RSSI on Thread (mesh-side proxy for the WiFi RSSI in ESP32). */
	int parent_rssi = 0;
	{
		int8_t rssi = 0;
		otInstance *inst = openthread_get_default_instance();
		if (inst && otThreadGetParentAverageRssi(inst, &rssi) == OT_ERROR_NONE) {
			parent_rssi = rssi;
		}
	}
	n += snprintf(body + n, sizeof(body) - n, "%d,", parent_rssi);

	n += snprintf(body + n, sizeof(body) - n,
		      "<span class='G'>READY</span>,%.0f,",
		      (double)snap.last_rssi);

	n += snprintf(body + n, sizeof(body) - n, "%s,",
		      snap.satellite[0] ? snap.satellite : "-");

	/* Sat lat/lon, az/el — populated by doppler_update() in main.cpp.
	 * tle_valid is set when we've received a real TLE; before that
	 * fall back to "-" so the dashboard doesn't show stale zero values. */
	if (snap.tle_valid) {
		n += snprintf(body + n, sizeof(body) - n,
			      "%.2f / %.2f,%.0f / %.1f,",
			      (double)snap.sat_lat, (double)snap.sat_lon,
			      (double)snap.sat_azimuth, (double)snap.sat_elevation);
	} else {
		n += snprintf(body + n, sizeof(body) - n, "- / -,- / -,");
	}

	n += snprintf(body + n, sizeof(body) - n, "%.0f Hz,",
		      (double)snap.freq_doppler);

	/* UTC + local time. We treat "local" as UTC because the device
	 * doesn't carry a TZ — the ESP32 doesn't either in many builds. */
	time_t now = time(NULL);
	if (now > 0) {
		struct tm tm_utc;
		gmtime_r(&now, &tm_utc);
		n += snprintf(body + n, sizeof(body) - n,
			      "%02d:%02d:%02d,%02d:%02d:%02d,",
			      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
			      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
	} else {
		n += snprintf(body + n, sizeof(body) - n, "-,-,");
	}

	/* Last packet timestamp — emit time-since in seconds for readability,
	 * or "-" if no packet received yet this boot. */
	if (snap.last_packet_uptime_ms > 0) {
		int64_t age_s = (k_uptime_get() - snap.last_packet_uptime_ms) / 1000;
		n += snprintf(body + n, sizeof(body) - n, "%llds ago,",
			      (long long)age_s);
	} else {
		n += snprintf(body + n, sizeof(body) - n, "-,");
	}

	n += snprintf(body + n, sizeof(body) - n, "%.0f,%.1f,%.0f,%s\n",
		      (double)snap.last_rssi,
		      (double)snap.last_snr,
		      (double)snap.last_freq_err,
		      snap.last_crc_error ? "CRC ERROR!" : "");

	if (n < 0) n = 0;
	if (n > (int)sizeof(body)) n = sizeof(body);

	response_ctx->body = (uint8_t *)body;
	response_ctx->body_len = (size_t)n;
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic wm_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/plain",
	},
	.cb = wm_handler,
	.user_data = NULL,
};

/* ===== Service + resource registration =====
 *
 * Service definitions live at file scope so the linker iterable-section
 * macros can collect them at compile time. */
static uint16_t web_service_port = 80;

HTTP_SERVICE_DEFINE(tinygs_web, NULL, &web_service_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(tinygs_root, tinygs_web, "/", &root_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_restart, tinygs_web, "/restart", &restart_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_cs, tinygs_web, "/cs", &cs_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_wm, tinygs_web, "/wm", &wm_resource_detail);

/* ===== SRP service registration =====
 *
 * Publishes `_http._tcp` on port 80 with instance name `tinygs-<EUI suffix>`
 * to the BR's SRP server. HA's OTBR proxies this to mDNS on the LAN side
 * so a host can reach the device as `tinygs-<id>._http._tcp.local`.
 *
 * All strings passed to OpenThread must outlive the call — they're held by
 * the SRP client linked-list. We use file-scope static storage. */
static char srp_host_name[24];        /* "tinygs-46BC7D" */
static char srp_instance_name[40];    /* "tinygs-46BC7D" */
static otSrpClientService srp_http_service;
static bool srp_registered;

static void srp_callback(otError aError, const otSrpClientHostInfo *,
			 const otSrpClientService *,
			 const otSrpClientService *,
			 void *)
{
	if (aError == OT_ERROR_NONE) {
		LOG_INF("SRP: registered _http._tcp.%s on port 80", srp_instance_name);
	} else {
		LOG_WRN("SRP callback error %d", aError);
	}
}

static void srp_autostart_callback(const otSockAddr *aServerSockAddr, void *)
{
	if (aServerSockAddr) {
		LOG_INF("SRP server discovered, auto-start active");
	} else {
		LOG_INF("SRP server lost, auto-start paused");
	}
}

/* Sanitize a station name to a DNS-label (RFC 1035): lowercase letters,
 * digits, and `-`; max 63 chars; can't start or end with `-`. ESP32 TinyGS
 * uses the station name verbatim — we apply the same rule, but defensively
 * because cfg_station is user-editable and may have spaces or unicode. */
static void sanitize_dns_label(const char *src, char *dst, size_t cap)
{
	if (cap == 0) {
		return;
	}
	size_t out = 0;
	bool last_dash = true; /* prevents leading dash */
	for (size_t i = 0; src && src[i] && out < cap - 1; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (ok) {
			dst[out++] = (char)c;
			last_dash = false;
		} else if (!last_dash) {
			dst[out++] = '-';
			last_dash = true;
		}
	}
	while (out > 0 && dst[out - 1] == '-') {
		out--;  /* strip trailing dashes */
	}
	dst[out] = '\0';
}

static int srp_register(void)
{
	if (srp_registered) {
		return 0;
	}
	otInstance *inst = openthread_get_default_instance();
	if (!inst) {
		return -ENODEV;
	}

	/* Prefer the configured station name (matches ESP32 TinyGS behaviour).
	 * Fall back to a stable EUI-derived suffix when the user hasn't set
	 * one. The station name is sanitised to a DNS label since cfg_station
	 * is user-editable and could contain spaces or other invalid bytes. */
	char sanitized[24];
	sanitize_dns_label(cfg_station, sanitized, sizeof(sanitized));

	if (sanitized[0] != '\0') {
		snprintf(srp_host_name, sizeof(srp_host_name), "%s", sanitized);
	} else {
		otExtAddress eui;
		otLinkGetFactoryAssignedIeeeEui64(inst, &eui);
		snprintf(srp_host_name, sizeof(srp_host_name), "tinygs-%02x%02x%02x",
			 eui.m8[5], eui.m8[6], eui.m8[7]);
	}
	snprintf(srp_instance_name, sizeof(srp_instance_name), "%s", srp_host_name);

	otError err = otSrpClientSetHostName(inst, srp_host_name);
	if (err != OT_ERROR_NONE && err != OT_ERROR_INVALID_STATE) {
		LOG_ERR("SRP set host name failed: %d", err);
		return -EIO;
	}

	err = otSrpClientEnableAutoHostAddress(inst);
	if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
		LOG_ERR("SRP enable auto host address failed: %d", err);
		return -EIO;
	}

	srp_http_service.mName         = "_http._tcp";
	srp_http_service.mInstanceName = srp_instance_name;
	srp_http_service.mPort         = 80;
	srp_http_service.mPriority     = 0;
	srp_http_service.mWeight       = 0;
	srp_http_service.mNumTxtEntries = 0;
	srp_http_service.mTxtEntries    = NULL;
	srp_http_service.mSubTypeLabels = NULL;
	srp_http_service.mLease         = 0;  /* server default */
	srp_http_service.mKeyLease      = 0;

	err = otSrpClientAddService(inst, &srp_http_service);
	if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
		LOG_ERR("SRP add service failed: %d", err);
		return -EIO;
	}

	otSrpClientSetCallback(inst, srp_callback, NULL);
	otSrpClientEnableAutoStartMode(inst, srp_autostart_callback, NULL);

	srp_registered = true;
	LOG_INF("SRP: published %s.%s port 80 (autostart on)",
		srp_instance_name, srp_http_service.mName);
	return 0;
}

/* ===== Lifecycle ===== */
static bool started;

int web_ui_start(void)
{
	if (started) {
		return 0;
	}
	k_work_init_delayable(&restart_work, restart_work_handler);
	int rc = http_server_start();
	if (rc != 0) {
		LOG_ERR("http_server_start failed: %d", rc);
		return rc;
	}
	(void)srp_register();
	started = true;
	LOG_INF("Web UI on :80 (resources: /, /restart, /cs?c2=<seq>, /wm)");
	return 0;
}
