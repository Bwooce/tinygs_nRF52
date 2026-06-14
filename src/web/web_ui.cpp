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
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>           /* strcasecmp / strncasecmp */
#include <zephyr/sys/base64.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>

#include "web_ui.h"
#include "tinygs_protocol.h"   /* TINYGS_VERSION, tinygs_radio */
#include "tinygs_config.h"     /* cfg_station[], cfg_admin_pw[], etc. */
#include "tinygs_tz.h"         /* TZ table for the /config dropdown */
#include "dashboard_html_gz.h" /* DASHBOARD_HTML_GZ[] — pre-gzipped */
#include "favicon_png.h"       /* FAVICON_PNG[] */

/* picolibc <time.h> only exposes localtime_r under POSIX feature macros that
 * Zephyr doesn't enable globally. Declare it ourselves; the linker pulls in
 * the picolibc symbol directly. */
extern "C" struct tm *localtime_r(const time_t *, struct tm *);

/* Probe result from main.cpp — gates ext-flash use throughout the app. */
extern "C" bool tinygs_ext_flash_present(void);
#include "logo_png.h"          /* LOGO_PNG[] — 310x149 TinyGS banner */
#include "tinygs_display.h"    /* tinygs_display_request_weblogin */

/* MQTT connection state, exposed by main.cpp via a small accessor so we
 * don't need to extern the file-local `enum app_state` (declarations would
 * have to match exactly across TUs and would silently break on reordering). */
extern "C" bool tinygs_mqtt_is_connected(void);

/* Battery + USB-detect, defined in main.cpp. read_vbat_mv() returns
 * battery voltage in millivolts via the on-board ADC + divider;
 * usb_vbus_present() polls NRF_POWER->USBREGSTATUS. Both used in the
 * /wm Power row. */
extern "C" int read_vbat_mv(void);
extern "C" bool tinygs_usb_vbus_present(void);

/* Capture the Authorization header on every request so the auth helper
 * below can inspect it. The capture infrastructure is on a per-server
 * basis, so registering once here covers all our resources. */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(authz_capture, "Authorization");

/* ===== HTTP Basic auth ============================================= */

/* Compare against cfg_admin_pw with a fixed `admin` username. Returns
 * true when the request header matches; returns false (and the caller
 * should send 401) otherwise.
 *
 * Decoded base64 may contain NULs or non-printables — we treat the
 * whole thing as opaque bytes and compare. */
static bool basic_auth_ok(struct http_client_ctx *client)
{
	const struct http_header_capture_ctx *hc = &client->header_capture_ctx;
	const char *value = NULL;
	for (size_t i = 0; i < hc->count; i++) {
		if (strcasecmp(hc->headers[i].name, "Authorization") == 0) {
			value = hc->headers[i].value;
			break;
		}
	}
	if (!value) {
		return false;
	}
	/* Expect "Basic <b64>" */
	while (*value == ' ') value++;
	if (strncasecmp(value, "Basic ", 6) != 0) {
		return false;
	}
	const char *b64 = value + 6;
	while (*b64 == ' ') b64++;

	uint8_t decoded[96];
	size_t decoded_len = 0;
	if (base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
			  (const uint8_t *)b64, strlen(b64)) != 0) {
		return false;
	}
	decoded[decoded_len] = '\0';
	const char *colon = (const char *)memchr(decoded, ':', decoded_len);
	if (!colon) {
		return false;
	}
	size_t user_len = (size_t)(colon - (char *)decoded);
	const char *pw = colon + 1;
	size_t pw_len = decoded_len - user_len - 1;

	if (user_len != 5 || memcmp(decoded, "admin", 5) != 0) {
		return false;
	}
	size_t expect_len = strlen(cfg_admin_pw);
	if (pw_len != expect_len) {
		return false;
	}
	return memcmp(pw, cfg_admin_pw, pw_len) == 0;
}

/* Send a 401 with WWW-Authenticate so the browser prompts for a password.
 * Caller must return immediately after this. */
static void send_401(struct http_response_ctx *response_ctx)
{
	static const struct http_header www_auth_headers[] = {
		{ .name = "WWW-Authenticate",
		  .value = "Basic realm=\"TinyGS\"" },
	};
	static const char body[] = "Unauthorized\n";
	response_ctx->status = HTTP_401_UNAUTHORIZED;
	response_ctx->headers = www_auth_headers;
	response_ctx->header_count = ARRAY_SIZE(www_auth_headers);
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = sizeof(body) - 1;
	response_ctx->final_chunk = true;
}

/* ===== form-urlencoded parser =====================================
 * Extract a single field by name from a body of `key=val&key=val&...`.
 * URL-decodes the value into `out` (NUL-terminated). Returns true on hit. */
static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool form_get(const char *body, size_t body_len,
		     const char *key, char *out, size_t out_cap)
{
	if (out_cap == 0) return false;
	out[0] = '\0';
	size_t key_len = strlen(key);
	const char *p = body;
	const char *end = body + body_len;
	while (p < end) {
		const char *eq = (const char *)memchr(p, '=', end - p);
		if (!eq) break;
		size_t this_key = eq - p;
		const char *amp = (const char *)memchr(eq, '&', end - eq);
		if (!amp) amp = end;

		if (this_key == key_len && memcmp(p, key, key_len) == 0) {
			size_t n = 0;
			const char *v = eq + 1;
			while (v < amp && n < out_cap - 1) {
				char c = *v++;
				if (c == '+') {
					out[n++] = ' ';
				} else if (c == '%' && v + 1 < amp) {
					int hi = hex_nibble(v[0]);
					int lo = hex_nibble(v[1]);
					if (hi >= 0 && lo >= 0) {
						out[n++] = (char)((hi << 4) | lo);
						v += 2;
					} else {
						out[n++] = c;
					}
				} else {
					out[n++] = c;
				}
			}
			out[n] = '\0';
			return true;
		}
		p = amp + 1;
	}
	return false;
}

/* Provided by Zephyr OpenThread integration. Forward-declared so we don't
 * have to drag in zephyr/net/openthread.h (deprecated wrappers). */
extern "C" struct otInstance *openthread_get_default_instance(void);

LOG_MODULE_REGISTER(web_ui, LOG_LEVEL_INF);

/* ===== / handler — IoTWebConf-style home page =====
 *
 * Matches the ESP32 TinyGS root page shape: brief station header +
 * navigation buttons to dashboard / config / firmware / restart.
 * Endpoints we don't implement yet (config, firmware) render as
 * disabled buttons so the page still looks complete.
 *
 * For programmatic clients (curl, monitoring scripts, Phase 4 health
 * checks), keep the plain-text view available at /status. */
static int root_handler(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx,
			void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	/* Sized to fit the full HTML — initial 1024-byte buffer was being
	 * silently truncated mid-button. snprintf returns the would-be
	 * length on overflow, which we now check and log. */
	static char body[1536];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	int n = snprintf(body, sizeof(body),
		"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1'>"
		"<title>TinyGS nRF52</title>"
		"<style>"
		"body{font-family:Arial,sans-serif;margin:0;padding:20px;"
		"text-align:center;max-width:480px;margin:0 auto;}"
		"h1{font-size:1.4em;margin:8px 0;}"
		"p.sub{color:#666;margin:0 0 20px 0;font-size:0.9em;}"
		"a.btn{display:block;width:100%%;max-width:300px;"
		"margin:8px auto;padding:14px;font-size:1em;border:1px solid #888;"
		"border-radius:6px;background:#f4f4f4;color:#222;text-decoration:none;"
		"box-sizing:border-box;cursor:pointer;}"
		"a.btn:hover{background:#e8e8e8;}"
		"a.btn.disabled{color:#aaa;background:#f8f8f8;cursor:not-allowed;}"
		"a.danger{color:#a00;border-color:#a00;}"
		"img.logo{max-width:100%%;height:auto;display:block;margin:0 auto 10px;}"
		"</style></head><body>"
		"<img class='logo' src='/logo.png' alt='TinyGS'>"
		"<h1>%s</h1>"
		"<p class='sub'>TinyGS nRF52 v%u &middot; uptime %llds &middot; log_seq %u</p>"
		"<a class='btn' href='/dashboard'>Dashboard</a>"
		"<a class='btn' href='/config'>Configure</a>"
		"<a class='btn disabled' title='Phase 5'>Firmware Update</a>"
		"<a class='btn danger' href='/restart' "
		"onclick=\"return confirm('Reboot?')\">Restart</a>"
		"<p class='sub'><a href='/status'>raw status</a></p>"
		"</body></html>",
		cfg_station[0] ? cfg_station : "tinygs",
		(unsigned)TINYGS_VERSION,
		(long long)(k_uptime_get() / 1000),
		web_log_head_seq());
	if (n < 0) {
		n = 0;
	} else if (n >= (int)sizeof(body)) {
		LOG_WRN("/ truncated: needed %d bytes, have %u",
			n, (unsigned)sizeof(body));
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
		.content_type = "text/html",
	},
	.cb = root_handler,
	.user_data = NULL,
};

/* ===== /status handler — original plain-text status =====
 *
 * What `/` used to be. Kept as a separate endpoint so monitoring
 * scripts / health checks have a stable text response that's easy to
 * grep, while the browser-facing root serves HTML. */
static int status_handler(struct http_client_ctx *client,
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
			 "log_seq: %u\n"
			 "ext_flash: %d\n",
			 cfg_station[0] ? cfg_station : "tinygs",
			 (unsigned)TINYGS_VERSION,
			 (long long)k_uptime_get(),
			 web_log_head_seq(),
			 tinygs_ext_flash_present() ? 1 : 0);
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

static struct http_resource_detail_dynamic status_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_HEAD),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/plain",
	},
	.cb = status_handler,
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
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	static const char body[] = "Rebooting in 2 seconds.\n";

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (!basic_auth_ok(client)) {
		send_401(response_ctx);
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
 *
 * When `c1=!cmd` is also present the request is treated as a write — Basic
 * auth is required. Supported commands mirror ESP32's:
 *   !e — soft reboot (deferred 2s like /restart)
 *   !w — request weblogin URL from the server
 *   !p — request a test LoRa TX (gated on cfg_tx_enable; emits log line)
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

/* Pull `c1=<value>` from the URL (URL-decoded into `out`). Returns true
 * when the parameter is present and non-empty. The value is at most 31
 * chars in the form (we never need more than `!XX` plus padding). */
static bool parse_cmd_from_query(const char *url_buf, size_t url_len,
				 char *out, size_t out_cap)
{
	if (out_cap == 0) return false;
	out[0] = '\0';
	if (!url_buf || url_len == 0) return false;
	for (size_t i = 0; i + 3 < url_len; i++) {
		if (url_buf[i] == 'c' && url_buf[i+1] == '1' && url_buf[i+2] == '=') {
			size_t n = 0, j = i + 3;
			while (j < url_len && out_cap > 1 && n < out_cap - 1) {
				char c = url_buf[j];
				if (c == '&' || c == '\0' || c == ' ') break;
				if (c == '+') {
					out[n++] = ' ';
				} else if (c == '%' && j + 2 < url_len) {
					int hi = hex_nibble(url_buf[j+1]);
					int lo = hex_nibble(url_buf[j+2]);
					if (hi >= 0 && lo >= 0) {
						out[n++] = (char)((hi << 4) | lo);
						j += 2;
					} else {
						out[n++] = c;
					}
				} else {
					out[n++] = c;
				}
				j++;
			}
			out[n] = '\0';
			return n > 0;
		}
	}
	return false;
}

/* Latches set by /cs for the main loop to act on. Polled from main.cpp
 * via the public accessors below. */
static volatile bool web_test_packet_requested = false;
static volatile bool web_reboot_requested = false;

extern "C" bool web_ui_pop_test_packet_request(void)
{
	if (web_test_packet_requested) {
		web_test_packet_requested = false;
		return true;
	}
	return false;
}

extern "C" bool web_ui_pop_reboot_request(void)
{
	if (web_reboot_requested) {
		web_reboot_requested = false;
		return true;
	}
	return false;
}

/* Run a /cs command. Returns a short status string suitable for echo
 * back to the console. */
static const char *run_cs_command(const char *cmd)
{
	if (strcmp(cmd, "!e") == 0) {
		web_reboot_requested = true;
		LOG_WRN("/cs !e: reboot requested via web UI");
		return "reboot scheduled\n";
	}
	if (strcmp(cmd, "!w") == 0) {
		tinygs_display_request_weblogin();
		LOG_INF("/cs !w: weblogin requested via web UI");
		return "weblogin URL requested from server\n";
	}
	if (strcmp(cmd, "!p") == 0) {
		extern int8_t cfg_tx_enable;
		if (!cfg_tx_enable) {
			return "tx disabled (cfg_tx_enable=0)\n";
		}
		web_test_packet_requested = true;
		LOG_INF("/cs !p: test packet requested via web UI");
		return "test packet queued\n";
	}
	return "unknown command\n";
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

	/* If a c1=<command> is present, this is a write — auth required.
	 * Run the command, echo a status line into the response so the
	 * dashboard's textarea reflects what happened. */
	char cmd[32];
	const char *cmd_status = NULL;
	if (parse_cmd_from_query((const char *)client->url_buffer,
				 sizeof(client->url_buffer), cmd, sizeof(cmd))) {
		if (!basic_auth_ok(client)) {
			send_401(response_ctx);
			return 0;
		}
		cmd_status = run_cs_command(cmd);
	}

	uint32_t high = since;

	/* Reserve a fixed 16-byte prefix for the header. This avoids the
	 * snprintf NUL terminator clobbering the first byte of the log
	 * content when header and content share one buffer. */
	enum { HDR_RESERVE = 16 };
	int wrote = web_log_read_since(since,
				       body + HDR_RESERVE,
				       (int)sizeof(body) - HDR_RESERVE,
				       &high);

	/* Append the command status (if any) after the log lines. */
	if (cmd_status) {
		size_t status_len = strlen(cmd_status);
		if (wrote + (int)status_len + HDR_RESERVE < (int)sizeof(body)) {
			memcpy(body + HDR_RESERVE + wrote, cmd_status, status_len);
			wrote += (int)status_len;
		}
	}

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

/* ===== /config handler — GET form, POST save ===================== */

/* The /config form is rendered as a multi-chunk HTTP response so the 460
 * timezone <option> rows fit without bloating body[]: the handler is called
 * repeatedly (Zephyr's dynamic-resource pattern, see http_server_http1.c
 * around line 500), each call returns the next chunk with final_chunk=false
 * until the very last one. State lives in config_get_chunk_state below. */
#define TZ_OPTIONS_PER_CHUNK 30  /* ~30 * 50 B = 1.5 KB per chunk, fits in body[3072] */

/* Escape src for embedding inside a single-quote-delimited HTML attribute.
 * Replaces ', &, <, > with their named entities; everything else passes
 * through. Always NUL-terminates if dstlen >= 1. dst may be NULL to just
 * measure the needed size (return value). */
static size_t html_attr_escape(char *dst, size_t dstlen, const char *src)
{
	size_t n = 0;
	const char *ent;
	while (*src) {
		switch (*src) {
		case '&':  ent = "&amp;";  break;
		case '\'': ent = "&#39;";  break;
		case '<':  ent = "&lt;";   break;
		case '>':  ent = "&gt;";   break;
		default:   ent = NULL;     break;
		}
		if (ent) {
			size_t elen = strlen(ent);
			if (dst && n + elen < dstlen) {
				memcpy(dst + n, ent, elen);
			}
			n += elen;
		} else {
			if (dst && n + 1 < dstlen) { dst[n] = *src; }
			n += 1;
		}
		src++;
	}
	if (dst && dstlen > 0) {
		dst[(n < dstlen) ? n : dstlen - 1] = '\0';
	}
	return n;
}

/* Header chunk: everything up to <select name='tz'>. */
static int render_config_form_header(char *out, int cap)
{
	extern float tinygs_station_lat;
	extern float tinygs_station_lon;
	extern float tinygs_station_alt;

	/* Worst case: every char of cfg_station[32] / cfg_mqtt_user[64] becomes
	 * a 5-char entity (&#39;/&amp;), so 5x + NUL. Stack-resident, scoped
	 * to this single render call. */
	char esc_station[5 * sizeof(cfg_station) + 1];
	char esc_mqtt_user[5 * sizeof(cfg_mqtt_user) + 1];
	html_attr_escape(esc_station, sizeof(esc_station), cfg_station);
	html_attr_escape(esc_mqtt_user, sizeof(esc_mqtt_user), cfg_mqtt_user);

	int n = snprintf(out, (size_t)cap,
		"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1'>"
		"<title>TinyGS Config</title><style>"
		"body{font-family:Arial;margin:0;padding:14px;max-width:520px;margin:0 auto;}"
		"h1{font-size:1.2em;margin:0 0 10px 0;text-align:center;}"
		"label{display:block;margin:10px 0 2px 0;font-size:0.9em;color:#444;}"
		"input[type=text],input[type=number],input[type=password],select{"
		"width:100%%;padding:8px;font-size:1em;border:1px solid #888;"
		"border-radius:4px;box-sizing:border-box;}"
		"input[type=submit]{margin-top:14px;padding:12px;width:100%%;"
		"background:#3a6;color:#fff;border:none;border-radius:6px;"
		"font-size:1em;cursor:pointer;}"
		"input[type=submit]:hover{background:#285;}"
		".note{color:#888;font-size:0.8em;margin:4px 0 0 0;}"
		".row{display:flex;gap:10px;}.row>div{flex:1;}"
		"</style></head><body>"
		"<h1>TinyGS Configuration</h1>"
		"<form method='POST' action='/config'>"
		"<label>Station name<input type='text' name='station' value='%s' maxlength='31'></label>"
		"<div class='row'>"
		"<div><label>Latitude<input type='number' step='0.0001' name='lat' value='%.4f'></label></div>"
		"<div><label>Longitude<input type='number' step='0.0001' name='lon' value='%.4f'></label></div>"
		"<div><label>Altitude (m)<input type='number' step='1' name='alt' value='%.0f'></label></div>"
		"</div>"
		"<label>MQTT username<input type='text' name='mqtt_user' value='%s' maxlength='63'></label>"
		"<label>MQTT password<input type='password' name='mqtt_pass' placeholder='unchanged' maxlength='63'></label>"
		"<p class='note'>Leave blank to keep current MQTT password.</p>"
		"<label>Admin password (web UI)<input type='password' name='admin_pw' placeholder='unchanged' maxlength='31'></label>"
		"<p class='note'>Used for /config and /restart. Default is &quot;tinygs&quot;.</p>"
		"<label>Timezone<select name='tz'>",
		esc_station,
		(double)tinygs_station_lat,
		(double)tinygs_station_lon,
		(double)tinygs_station_alt,
		esc_mqtt_user);
	return (n < 0) ? 0 : (n > cap ? cap : n);
}

/* Render one batch of <option> rows starting at start_idx, up to
 * TZ_OPTIONS_PER_CHUNK or until the array ends. Marks the current
 * cfg_tz_idx with 'selected'. */
static int render_config_form_tz_options(char *out, int cap, size_t start_idx)
{
	size_t end = start_idx + TZ_OPTIONS_PER_CHUNK;
	if (end > tinygs_tz_count) end = tinygs_tz_count;

	int pos = 0;
	for (size_t i = start_idx; i < end; i++) {
		const char *name = tinygs_tz_get_name((uint16_t)i);
		int wrote = snprintf(out + pos, cap - pos,
			"<option value=%u%s>%s",
			(unsigned)i,
			i == cfg_tz_idx ? " selected" : "",
			name);
		if (wrote < 0 || pos + wrote >= cap) {
			break;
		}
		pos += wrote;
	}
	return pos;
}

/* Footer chunk: close the select + remaining form fields. */
static int render_config_form_footer(char *out, int cap)
{
	extern int8_t cfg_tx_enable;
	int n = snprintf(out, (size_t)cap,
		"</select></label>"
		"<label><input type='checkbox' name='tx' value='1'%s> Allow TX (RF transmit). Operator responsible for licensing.</label>"
		"<input type='submit' value='Save (reboot to apply MQTT changes)'>"
		"</form>"
		"<p class='note'><a href='/'>back to home</a></p>"
		"</body></html>",
		cfg_tx_enable ? " checked" : "");
	return (n < 0) ? 0 : (n > cap ? cap : n);
}

static int config_handler(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx,
			  void *user_data)
{
	ARG_UNUSED(user_data);

	/* Auth check on the very first chunk so we can 401 immediately. */
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		/* Body chunks may arrive before we get the final flag; we
		 * accumulate POST data here. */
		if (client->method == HTTP_POST) {
			static uint8_t post_buf[1024];
			static size_t post_len;
			if (request_ctx && request_ctx->data_len > 0 &&
			    post_len + request_ctx->data_len <= sizeof(post_buf)) {
				memcpy(post_buf + post_len, request_ctx->data,
				       request_ctx->data_len);
				post_len += request_ctx->data_len;
			}
			/* Stash so the FINAL pass can read it; abuse user_data
			 * via static scope. Cleared at FINAL. */
		}
		return 0;
	}

	if (!basic_auth_ok(client)) {
		send_401(response_ctx);
		return 0;
	}

	static char body[3072];

	if (client->method == HTTP_POST) {
		/* The static buffers from the chunk path are reused here. */
		extern float tinygs_station_lat;
		extern float tinygs_station_lon;
		extern float tinygs_station_alt;
		extern int8_t cfg_tx_enable;
		static uint8_t post_buf[1024];
		static size_t post_len;
		/* Append any FINAL-chunk data still being delivered. */
		if (request_ctx && request_ctx->data_len > 0 &&
		    post_len + request_ctx->data_len <= sizeof(post_buf)) {
			memcpy(post_buf + post_len, request_ctx->data,
			       request_ctx->data_len);
			post_len += request_ctx->data_len;
		}

		char field[256];
		bool changes = false;
		bool mqtt_changed = false;

		if (form_get((const char *)post_buf, post_len, "station", field, sizeof(field))
		    && field[0]) {
			if (strncmp(cfg_station, field, sizeof(cfg_station)) != 0) {
				strncpy(cfg_station, field, sizeof(cfg_station) - 1);
				cfg_station[sizeof(cfg_station) - 1] = '\0';
				tinygs_config_save_station(cfg_station);
				changes = true;
				mqtt_changed = true;
				LOG_INF("/config: station -> %s", cfg_station);
			}
		}
		if (form_get((const char *)post_buf, post_len, "lat", field, sizeof(field))
		    && field[0]) {
			float v = strtof(field, NULL);
			if (v != tinygs_station_lat) {
				tinygs_station_lat = v;
				changes = true;
			}
		}
		if (form_get((const char *)post_buf, post_len, "lon", field, sizeof(field))
		    && field[0]) {
			float v = strtof(field, NULL);
			if (v != tinygs_station_lon) {
				tinygs_station_lon = v;
				changes = true;
			}
		}
		if (form_get((const char *)post_buf, post_len, "alt", field, sizeof(field))
		    && field[0]) {
			float v = strtof(field, NULL);
			if (v != tinygs_station_alt) {
				tinygs_station_alt = v;
				changes = true;
			}
		}
		if (changes) {
			tinygs_config_save_location(tinygs_station_lat,
						    tinygs_station_lon,
						    tinygs_station_alt);
		}

		if (form_get((const char *)post_buf, post_len, "mqtt_user", field, sizeof(field))
		    && field[0]) {
			if (strncmp(cfg_mqtt_user, field, sizeof(cfg_mqtt_user)) != 0) {
				strncpy(cfg_mqtt_user, field, sizeof(cfg_mqtt_user) - 1);
				cfg_mqtt_user[sizeof(cfg_mqtt_user) - 1] = '\0';
				tinygs_config_save("user", cfg_mqtt_user, strlen(cfg_mqtt_user));
				mqtt_changed = true;
				LOG_INF("/config: mqtt_user -> (set, %zu chars)",
					strlen(cfg_mqtt_user));
			}
		}
		if (form_get((const char *)post_buf, post_len, "mqtt_pass", field, sizeof(field))
		    && field[0]) {
			strncpy(cfg_mqtt_pass, field, sizeof(cfg_mqtt_pass) - 1);
			cfg_mqtt_pass[sizeof(cfg_mqtt_pass) - 1] = '\0';
			tinygs_config_save("pass", cfg_mqtt_pass, strlen(cfg_mqtt_pass));
			mqtt_changed = true;
			LOG_INF("/config: mqtt_pass -> (changed, %zu chars)",
				strlen(cfg_mqtt_pass));
		}

		if (form_get((const char *)post_buf, post_len, "admin_pw", field, sizeof(field))
		    && field[0]) {
			strncpy(cfg_admin_pw, field, sizeof(cfg_admin_pw) - 1);
			cfg_admin_pw[sizeof(cfg_admin_pw) - 1] = '\0';
			tinygs_config_save("adm", cfg_admin_pw, strlen(cfg_admin_pw));
			LOG_INF("/config: admin_pw changed");
		}

		if (form_get((const char *)post_buf, post_len, "tz", field, sizeof(field))
		    && field[0]) {
			char *end_p = NULL;
			unsigned long v = strtoul(field, &end_p, 10);
			if (end_p != field && v < tinygs_tz_count && (uint16_t)v != cfg_tz_idx) {
				cfg_tz_idx = (uint16_t)v;
				tinygs_config_save("tz", &cfg_tz_idx, sizeof(cfg_tz_idx));
				tinygs_tz_apply(cfg_tz_idx);
				LOG_INF("/config: tz -> %u (%s)", (unsigned)cfg_tz_idx,
					tinygs_tz_get_name(cfg_tz_idx));
			}
		}

		int8_t tx_new = 0;
		if (form_get((const char *)post_buf, post_len, "tx", field, sizeof(field))
		    && field[0]) {
			tx_new = (field[0] == '1') ? 1 : 0;
		}
		if (tx_new != cfg_tx_enable) {
			cfg_tx_enable = tx_new;
			tinygs_config_save("tx", &cfg_tx_enable, sizeof(cfg_tx_enable));
			LOG_INF("/config: tx_enable -> %d", cfg_tx_enable);
		}

		post_len = 0; /* reset for next request */

		const char *banner = mqtt_changed
			? "<p style='color:#a40'>Saved. Restart to apply MQTT/station changes.</p>"
			: "<p style='color:#080'>Saved.</p>";
		int n = snprintf(body, sizeof(body),
			"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
			"<meta http-equiv='refresh' content='2;url=/config'>"
			"<title>Saved</title></head><body>"
			"<p>%s</p>"
			"<p><a href='/config'>back to config</a> &middot; "
			"<a href='/'>home</a></p>"
			"</body></html>", banner);
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = (size_t)((n < 0) ? 0 : n);
		response_ctx->final_chunk = true;
		return 0;
	}

	/* GET — chunked render. The HTTP server calls this back repeatedly
	 * while final_chunk stays false. State below tracks our position
	 * through the form: 0 = emit header; 1..tinygs_tz_count = emit a
	 * batch of <option> rows starting at (state-1); > tinygs_tz_count
	 * = emit footer and reset. Static (not per-request) but the rest
	 * of the handler already shares mutable state via static body[]
	 * and post_buf[], so the contract is "one /config request at a
	 * time" — fine for an admin-only endpoint. */
	static size_t config_get_chunk_state;
	if (config_get_chunk_state == 0) {
		int n = render_config_form_header(body, sizeof(body));
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = (size_t)n;
		response_ctx->final_chunk = false;
		config_get_chunk_state = 1;
	} else if (config_get_chunk_state <= tinygs_tz_count) {
		size_t start_idx = config_get_chunk_state - 1;
		int n = render_config_form_tz_options(body, sizeof(body), start_idx);
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = (size_t)n;
		response_ctx->final_chunk = false;
		config_get_chunk_state += TZ_OPTIONS_PER_CHUNK;
	} else {
		int n = render_config_form_footer(body, sizeof(body));
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = (size_t)n;
		response_ctx->final_chunk = true;
		config_get_chunk_state = 0; /* reset for next request */
	}
	return 0;
}

static struct http_resource_detail_dynamic config_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.content_type = "text/html",
	},
	.cb = config_handler,
	.user_data = NULL,
};

/* ===== /favicon.ico — static PNG ================================== */
static struct http_resource_detail_static favicon_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_HEAD),
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.content_type = "image/png",
	},
	.static_data = (uint8_t *)FAVICON_PNG,
	.static_data_len = sizeof(FAVICON_PNG),
};

/* ===== /logo.png — 310x149 dashboard banner ======================= */
static struct http_resource_detail_static logo_resource_detail = {
	.common = {
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_HEAD),
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.content_type = "image/png",
	},
	.static_data = (uint8_t *)LOGO_PNG,
	.static_data_len = sizeof(LOGO_PNG),
};

/* ===== /wm handler — worldmap + status CSV =====
 *
 * Field layout matches the ESP32 fork's revised dashboard (commit
 * 91123a5 "fix(dashboard): align /wm data layout, add GNSS row,
 * Power: label" + e2accc6 JS offset alignment), so the same browser
 * JS works against both:
 *
 *   wmp[0..1]   cx, cy                         sat pixel position
 *   wmp[2..8]   modem (7 items):
 *               mode, freq, foff, noise_floor, sf|br, cr|fdev, bw
 *   wmp[9..15]  gsstatus (7 items):
 *               name, version, mqtt, parent_rssi, radio, GNSS, Power
 *   wmp[16..21] satdata (6): sat_name, lat/lon, az/el, doppler, utc, local
 *   wmp[22..25] lastpacket (4): time, rssi, snr, freq_err
 *   wmp[26]     crc_state ('' = OK, 'CRC ERROR!' otherwise)
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

	/* Sat pixel-coords mapped to our dashboard's 240×135 SVG viewBox.
	 * sat_pos_x/y are stored in ESP32-legacy 128×64 grid coords (kept
	 * for compatibility with the on-device ST7789V renderer); scale to
	 * the dashboard's coordinate system here so a single /wm output
	 * works for both consumers. */
	int cx = (int)(snap.sat_pos_x * 240.0f / 128.0f);
	int cy = (int)(snap.sat_pos_y * 135.0f / 64.0f);

	int n = 0;
	n += snprintf(body + n, sizeof(body) - n, "%d,%d,", cx, cy);

	/* === modemconfig: 7 items (mode, freq, foff, noise, sf|br, cr|fdev, bw) === */
	n += snprintf(body + n, sizeof(body) - n, "%s,%.4f,%.0f,",
		      snap.modem_mode[0] ? snap.modem_mode : "-",
		      (double)snap.frequency,
		      (double)snap.freq_offset);

	/* Noise floor — last_rssi when no recent packet, else last packet's
	 * RSSI is the closest proxy we have (an SX1262 idle-channel RSSI
	 * read is possible but would need to be plumbed through a non-
	 * blocking accessor; for now this matches the ESP32 fork's reading
	 * which is "last observed RSSI"). */
	n += snprintf(body + n, sizeof(body) - n, "%.0f,",
		      (double)snap.last_rssi);

	if (strcmp(snap.modem_mode, "LoRa") == 0) {
		n += snprintf(body + n, sizeof(body) - n, "%d,%d,",
			      snap.sf, snap.cr);
	} else {
		n += snprintf(body + n, sizeof(body) - n, "%.1f,%.0f,",
			      (double)snap.bitrate, (double)snap.freq_dev);
	}
	n += snprintf(body + n, sizeof(body) - n, "%.1f,",
		      (double)snap.bw);

	/* === gsstatus: 7 items (name, ver, mqtt, parent_rssi, radio, GNSS, Power) === */
	n += snprintf(body + n, sizeof(body) - n, "%s,%u,",
		      cfg_station[0] ? cfg_station : "tinygs",
		      (unsigned)TINYGS_VERSION);

	n += snprintf(body + n, sizeof(body) - n, "%s,",
		      mqtt_up ? "<span class='G'>CONNECTED</span>"
			      : "<span class='R'>NOT CONNECTED</span>");

	/* Parent RSSI on Thread (mesh-side proxy for the WiFi RSSI in ESP32).
	 * Render with units like the fork does. */
	int parent_rssi = 0;
	{
		int8_t rssi = 0;
		otInstance *inst = openthread_get_default_instance();
		if (inst && otThreadGetParentAverageRssi(inst, &rssi) == OT_ERROR_NONE) {
			parent_rssi = rssi;
		}
	}
	n += snprintf(body + n, sizeof(body) - n, "%d dBm,", parent_rssi);

	n += snprintf(body + n, sizeof(body) - n,
		      "<span class='G'>READY</span>,");

	/* GNSS — no module on the T114; placeholder. */
	n += snprintf(body + n, sizeof(body) - n, "-,");

	/* Power: USB/Sol|BAT V.VV V (P%). No charge-current sensor on the
	 * T114, so we don't render mA. Battery percentage is a crude
	 * linear estimate (LiPo: 4.20 V → 100 %, 3.30 V → 0 %; clamped). */
	{
		int vbat_mv = read_vbat_mv();
		bool vbus = tinygs_usb_vbus_present();
		int pct = ((vbat_mv - 3300) * 100) / (4200 - 3300);
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		n += snprintf(body + n, sizeof(body) - n,
			      "%s %.2fV %d%%,",
			      vbus ? "USB" : "BAT",
			      (double)vbat_mv / 1000.0, pct);
	}

	/* === satdata: 6 items === */
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

	/* Local + UTC time. cfg_tz_idx (POSIX TZ rule applied via tinygs_tz_apply
	 * at boot / on /config save) drives localtime_r; gmtime_r is always UTC. */
	time_t now = time(NULL);
	if (now > 0) {
		struct tm tm_local, tm_utc;
		localtime_r(&now, &tm_local);
		gmtime_r(&now, &tm_utc);
		n += snprintf(body + n, sizeof(body) - n,
			      "%02d:%02d:%02d,%02d:%02d:%02d,",
			      tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
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

/* ===== /dashboard handler — static HTML page =====
 *
 * Served as a static resource (no per-request work). The page polls
 * /cs and /wm via XHR every few seconds to populate live data; we
 * don't render any state into the HTML itself.
 */
static struct http_resource_detail_static dashboard_resource_detail = {
	.common = {
		/* HEAD bit is advertised but Zephyr's static-resource handler
		 * (handle_http1_static_resource) hard-codes GET-only and returns
		 * 405 for HEAD regardless of this bitmask — see
		 * subsys/net/lib/http/http_server_http1.c:~144. Browsers always
		 * GET, so this only affects `curl -I` and a few proxies.
		 * Upstream-patch territory; not worth a Zephyr fork today. */
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_HEAD),
		.type = HTTP_RESOURCE_TYPE_STATIC,
		/* Pre-gzipped at build time. Cuts /dashboard transfer from
		 * ~24 KB / 3.4 s to ~3.9 KB / 0.6 s over Thread MTD. Browser
		 * sees `Content-Encoding: gzip` and decompresses transparently. */
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = (uint8_t *)DASHBOARD_HTML_GZ,
	.static_data_len = sizeof(DASHBOARD_HTML_GZ),
};

/* ===== Service + resource registration =====
 *
 * Service definitions live at file scope so the linker iterable-section
 * macros can collect them at compile time. */
static uint16_t web_service_port = 80;

HTTP_SERVICE_DEFINE(tinygs_web, NULL, &web_service_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(tinygs_root, tinygs_web, "/", &root_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_status, tinygs_web, "/status", &status_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_restart, tinygs_web, "/restart", &restart_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_cs, tinygs_web, "/cs", &cs_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_wm, tinygs_web, "/wm", &wm_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_dashboard, tinygs_web, "/dashboard",
		     &dashboard_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_config, tinygs_web, "/config",
		     &config_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_favicon, tinygs_web, "/favicon.ico",
		     &favicon_resource_detail);
HTTP_RESOURCE_DEFINE(tinygs_logo, tinygs_web, "/logo.png",
		     &logo_resource_detail);

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
	LOG_INF("Web UI on :80 (/, /dashboard, /config, /status, /restart, /cs?c2=<seq>, /wm, /favicon.ico)");
	return 0;
}
