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

#include <openthread/srp_client.h>
#include <openthread/thread.h>

#include "web_ui.h"
#include "tinygs_protocol.h"   /* TINYGS_VERSION */
#include "tinygs_config.h"     /* cfg_station[] */

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

static int srp_register(void)
{
	if (srp_registered) {
		return 0;
	}
	otInstance *inst = openthread_get_default_instance();
	if (!inst) {
		return -ENODEV;
	}

	/* Compose a stable name from the lower 24 bits of the EUI-64. The full
	 * EUI is 8 bytes but for a human-readable mDNS label the last 3 give
	 * enough collision-resistance for one home network. */
	otExtAddress eui;
	otLinkGetFactoryAssignedIeeeEui64(inst, &eui);
	snprintf(srp_host_name, sizeof(srp_host_name), "tinygs-%02X%02X%02X",
		 eui.m8[5], eui.m8[6], eui.m8[7]);
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
	LOG_INF("Web UI on :80 (resources: /, /restart, /cs?c2=<seq>)");
	return 0;
}
