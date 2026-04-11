#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/dns_client.h>
#include <openthread/joiner.h>
#include <openthread/link.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/storage/disk_access.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-adc.h>
#include <hal/nrf_power.h>
#include <hal/nrf_ficr.h>

#include <mbedtls/debug.h>
#include <RadioLib.h>
#include "hal/Zephyr/ZephyrHal.h"
#include "tinygs_protocol.h"
#include "tinygs_config.h"

LOG_MODULE_REGISTER(tinygs_nrf52, LOG_LEVEL_DBG);

/* -------------------------------------------------------------------------- */
/* Heap / RAM instrumentation                                                  */
/* -------------------------------------------------------------------------- */

extern struct k_heap _system_heap;

static void log_heap_usage(const char *label)
{
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
    struct sys_memory_stats stats;
    if (sys_heap_runtime_stats_get(&_system_heap.heap, &stats) == 0) {
        LOG_INF("RAM [%s]: sys_heap used=%u free=%u max=%u",
                label,
                (unsigned)stats.allocated_bytes,
                (unsigned)stats.free_bytes,
                (unsigned)stats.max_allocated_bytes);
    }
#endif
}

/* -------------------------------------------------------------------------- */
/* Application State Machine                                                  */
/* -------------------------------------------------------------------------- */

enum app_state {
    STATE_WAIT_THREAD,
    STATE_DNS_RESOLVE,
    STATE_MQTT_CONNECT,
    STATE_MQTT_CONNECTED,
    STATE_ERROR,
};

static enum app_state app_state = STATE_WAIT_THREAD;
static volatile bool thread_attached = false;

/* -------------------------------------------------------------------------- */
/* MQTT Configuration                                                         */
/* -------------------------------------------------------------------------- */

#define MQTT_BROKER_HOSTNAME "mqtt.tinygs.com"
#define MQTT_BROKER_PORT     8883
#include "mqtt_credentials.h" /* MQTT_USERNAME, MQTT_PASSWORD defaults — gitignored */
#include "tinygs_ca_cert.h"   /* TinyGS server cert for TLS cipher suite config */
#define MQTT_TLS_SEC_TAG     1

/* Runtime config variables are in tinygs_config.cpp, declared in tinygs_config.h */

/* MQTT client and buffers */
static struct mqtt_client mqtt_client;
static uint8_t mqtt_rx_buf[256];
static uint8_t mqtt_tx_buf[256];
static struct sockaddr_storage broker_addr;

/* -------------------------------------------------------------------------- */
/* Hardware: Vext, SPI, LoRa pins                                             */
/* -------------------------------------------------------------------------- */

static const struct gpio_dt_spec vext_pwr = GPIO_DT_SPEC_GET(DT_ALIAS(vext), gpios);

static const struct spi_dt_spec lora_spi = SPI_DT_SPEC_GET(DT_NODELABEL(sx1262), SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec lora_cs = GPIO_DT_SPEC_GET_BY_IDX(DT_PARENT(DT_NODELABEL(sx1262)), cs_gpios, 0);
static const struct gpio_dt_spec lora_reset = GPIO_DT_SPEC_GET(DT_NODELABEL(sx1262), reset_gpios);
static const struct gpio_dt_spec lora_busy = GPIO_DT_SPEC_GET(DT_NODELABEL(sx1262), busy_gpios);
static const struct gpio_dt_spec lora_dio1 = GPIO_DT_SPEC_GET(DT_NODELABEL(sx1262), dio1_gpios);

static void enable_peripherals(void)
{
    if (device_is_ready(vext_pwr.port)) {
        gpio_pin_configure_dt(&vext_pwr, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&vext_pwr, 1);
        k_msleep(500);
        LOG_INF("Vext power enabled");
    }
}

/* -------------------------------------------------------------------------- */
/* Battery Voltage ADC                                                         */
/* -------------------------------------------------------------------------- */

/* Battery ADC: P0.04 (AIN2), bias enable on P0.06 (active HIGH).
 * Voltage divider 100k:390k → multiplier ~4.9
 * Use direct GPIO spec instead of DT_ALIAS to avoid DTS node issues. */
static const struct gpio_dt_spec adc_ctrl = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin = 6,
    .dt_flags = GPIO_ACTIVE_HIGH,
};

int read_vbat_mv(void)
{
    const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
    if (!device_is_ready(adc_dev)) {
        return 3700;
    }

    /* Enable the voltage divider bias circuit */
    if (device_is_ready(adc_ctrl.port)) {
        gpio_pin_configure_dt(&adc_ctrl, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&adc_ctrl, 1);
        k_msleep(2);
    }

    struct adc_channel_cfg ch_cfg = {
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id = 2,
        .input_positive = NRF_SAADC_AIN2,
    };
    adc_channel_setup(adc_dev, &ch_cfg);

    int16_t sample = 0;
    struct adc_sequence seq = {
        .channels = BIT(2),
        .buffer = &sample,
        .buffer_size = sizeof(sample),
        .resolution = 12,
    };

    int ret = adc_read(adc_dev, &seq);

    /* Disable bias to save power */
    if (device_is_ready(adc_ctrl.port)) {
        gpio_pin_set_dt(&adc_ctrl, 0);
    }

    if (ret != 0 || sample < 0) {
        LOG_WRN("ADC read failed: %d (sample=%d)", ret, sample);
        return 3700;
    }

    /* raw * (3600mV / 4096) * 4.9 */
    return (int)((float)sample * 3600.0f / 4096.0f * 4.9f);
}

/* -------------------------------------------------------------------------- */
/* 1200 Baud Reset (UF2 Bootloader Entry)                                     */
/* -------------------------------------------------------------------------- */

static void baudrate_reset_handler(const struct device *dev, uint32_t baudrate)
{
    if (baudrate == 1200) {
        /* No logging here — USB IRQ context, logging to USB would deadlock */
        nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
        sys_reboot(SYS_REBOOT_COLD);
    }
}

/* -------------------------------------------------------------------------- */
/* OpenThread                                                                  */
/* -------------------------------------------------------------------------- */

static const char *ot_role_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "Disabled";
    case OT_DEVICE_ROLE_DETACHED: return "Detached";
    case OT_DEVICE_ROLE_CHILD:    return "Child";
    case OT_DEVICE_ROLE_ROUTER:   return "Router";
    case OT_DEVICE_ROLE_LEADER:   return "Leader";
    default:                      return "Unknown";
    }
}

static void ot_state_changed_handler(otChangedFlags flags,
                                     struct openthread_context *ot_context,
                                     void *user_data)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(ot_context->instance);
        LOG_INF("Thread role: %s", ot_role_str(role));

        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
            role == OT_DEVICE_ROLE_LEADER) {
            thread_attached = true;
        } else {
            thread_attached = false;
        }
    }
}

static struct openthread_state_changed_cb ot_state_cb = {
    .state_changed_cb = ot_state_changed_handler,
};

static void dump_ot_dataset(struct openthread_context *ctx)
{
    otOperationalDataset dataset;
    otError err = otDatasetGetActive(ctx->instance, &dataset);
    if (err != OT_ERROR_NONE) {
        LOG_ERR("Failed to get active dataset: %d", (int)err);
        return;
    }

    LOG_INF("Active Dataset:");
    if (dataset.mComponents.mIsChannelPresent) {
        LOG_INF("  Channel: %u", dataset.mChannel);
    }
    if (dataset.mComponents.mIsPanIdPresent) {
        LOG_INF("  PAN ID: 0x%04x", dataset.mPanId);
    }
    if (dataset.mComponents.mIsExtendedPanIdPresent) {
        LOG_HEXDUMP_INF(dataset.mExtendedPanId.m8, 8, "  Ext PAN ID:");
    }
    if (dataset.mComponents.mIsNetworkKeyPresent) {
        LOG_HEXDUMP_INF(dataset.mNetworkKey.m8, 16, "  Network Key:");
    }
    if (dataset.mComponents.mIsNetworkNamePresent) {
        LOG_INF("  Network Name: %s", dataset.mNetworkName.m8);
    }
}

static void dump_ot_state(struct openthread_context *ctx)
{
    otInstance *inst = ctx->instance;

    LOG_INF("Thread state:");
    LOG_INF("  Role: %s", ot_role_str(otThreadGetDeviceRole(inst)));
    LOG_INF("  Interface up: %s", otIp6IsEnabled(inst) ? "yes" : "no");
    LOG_INF("  Thread enabled: %s", otThreadGetDeviceRole(inst) != OT_DEVICE_ROLE_DISABLED ? "yes" : "no");
    LOG_INF("  Channel: %u", otLinkGetChannel(inst));
    LOG_INF("  PAN ID: 0x%04x", otLinkGetPanId(inst));

    const otExtAddress *ext = otLinkGetExtendedAddress(inst);
    if (ext) {
        LOG_HEXDUMP_INF(ext->m8, 8, "  EUI-64:");
    }

    /* Print all unicast addresses */
    const otNetifAddress *addr = otIp6GetUnicastAddresses(inst);
    while (addr) {
        char buf[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(&addr->mAddress, buf, sizeof(buf));
        LOG_INF("  IPv6 addr: %s", buf);
        addr = addr->mNext;
    }
}

static void log_ot_diagnostics(void)
{
    struct openthread_context *ctx = openthread_get_default_context();
    openthread_api_mutex_lock(ctx);
    dump_ot_state(ctx);
    openthread_api_mutex_unlock(ctx);
}

static void joiner_callback(otError error, void *context)
{
    if (error == OT_ERROR_NONE) {
        LOG_INF("=== Joiner succeeded! Starting Thread... ===");
        struct openthread_context *ctx = openthread_get_default_context();
        otIp6SetEnabled(ctx->instance, true);
        otThreadSetEnabled(ctx->instance, true);
    } else {
        LOG_ERR("Joiner failed: %d", (int)error);
    }
}

static void init_openthread(void)
{
    struct openthread_context *ctx = openthread_get_default_context();

    LOG_INF("Starting OpenThread (Joiner mode)...");
    openthread_state_changed_cb_register(ctx, &ot_state_cb);
    openthread_start(ctx);

    k_msleep(500);

    openthread_api_mutex_lock(ctx);
    otInstance *inst = ctx->instance;

    /* Check if we already have a valid dataset (from a previous successful join) */
    otOperationalDataset dataset;
    otError err = otDatasetGetActive(inst, &dataset);

    if (err == OT_ERROR_NONE && dataset.mComponents.mIsNetworkKeyPresent) {
        LOG_INF("Found existing dataset — attaching directly");
        dump_ot_dataset(ctx);
        otIp6SetEnabled(inst, true);
        otThreadSetEnabled(inst, true);
    } else {
        LOG_INF("No valid dataset — starting Joiner with PSKd");
        otIp6SetEnabled(inst, true);

        err = otJoinerStart(inst,
                            CONFIG_OPENTHREAD_JOINER_PSKD,  /* PSKd */
                            NULL,   /* provisioning URL */
                            NULL,   /* vendor name */
                            NULL,   /* vendor model */
                            NULL,   /* vendor sw version */
                            NULL,   /* vendor data */
                            joiner_callback, NULL);

        if (err != OT_ERROR_NONE) {
            LOG_ERR("otJoinerStart failed: %d", (int)err);
        } else {
            LOG_INF("Joiner started, waiting for commissioner (PSKd: %s)...",
                    CONFIG_OPENTHREAD_JOINER_PSKD);
        }
    }

    dump_ot_state(ctx);
    openthread_api_mutex_unlock(ctx);
}

/* -------------------------------------------------------------------------- */
/* DNS Resolution via OpenThread DNS client (NAT64)                            */
/* -------------------------------------------------------------------------- */

static K_SEM_DEFINE(dns_sem, 0, 1);
static volatile int dns_result = -1;

static void dns_callback(otError aError, const otDnsAddressResponse *aResponse, void *aContext)
{
    if (aError != OT_ERROR_NONE) {
        LOG_ERR("OT DNS callback error: %d", (int)aError);
        dns_result = -1;
        k_sem_give(&dns_sem);
        return;
    }

    otIp6Address addr;
    uint32_t ttl;
    otError err = otDnsAddressResponseGetAddress(aResponse, 0, &addr, &ttl);
    if (err != OT_ERROR_NONE) {
        LOG_ERR("OT DNS no address in response: %d", (int)err);
        dns_result = -1;
        k_sem_give(&dns_sem);
        return;
    }

    /* Store resolved address into broker_addr */
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&broker_addr;
    memset(sin6, 0, sizeof(*sin6));
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(MQTT_BROKER_PORT);
    memcpy(&sin6->sin6_addr, &addr, sizeof(addr));

    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&addr, addr_str, sizeof(addr_str));
    LOG_INF("Resolved %s -> [%s]:%d (TTL=%u)",
            MQTT_BROKER_HOSTNAME, addr_str, MQTT_BROKER_PORT, (unsigned)ttl);

    dns_result = 0;
    k_sem_give(&dns_sem);
}

static int resolve_broker(void)
{
    struct openthread_context *ctx = openthread_get_default_context();

    LOG_INF("Resolving %s via OT DNS...", MQTT_BROKER_HOSTNAME);

    k_sem_reset(&dns_sem);
    dns_result = -1;

    /*
     * Resolve via nat64.net's public DNS64 resolver.
     * Returns globally-routable AAAA addresses (e.g. 2a00:1098:2c::5:9fc3:4a17).
     * No local NAT64 translator needed — nat64.net's infrastructure translates.
     */
    openthread_api_mutex_lock(ctx);

    /* Configure OT DNS to use nat64.net's DNS64 resolver */
    otDnsQueryConfig config;
    memset(&config, 0, sizeof(config));

    /* nat64.net DNS64: 2a00:1098:2c::1 port 53 */
    otIp6Address dnsServer;
    memset(&dnsServer, 0, sizeof(dnsServer));
    dnsServer.mFields.m16[0] = htons(0x2a00);
    dnsServer.mFields.m16[1] = htons(0x1098);
    dnsServer.mFields.m16[2] = htons(0x002c);
    /* m16[3..6] = 0 */
    dnsServer.mFields.m16[7] = htons(0x0001);

    config.mServerSockAddr.mAddress = dnsServer;
    config.mServerSockAddr.mPort = 53;
    config.mResponseTimeout = 10000; /* 10s — nat64.net is ~250ms away */
    config.mMaxTxAttempts = 3;
    config.mRecursionFlag = OT_DNS_FLAG_RECURSION_DESIRED;
    config.mNat64Mode = OT_DNS_NAT64_ALLOW;

    char dns_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&dnsServer, dns_str, sizeof(dns_str));
    LOG_INF("Resolving %s via nat64.net DNS64 [%s]:53 ...",
            MQTT_BROKER_HOSTNAME, dns_str);

    /* Query for AAAA — nat64.net DNS64 returns synthesized routable addresses */
    otError err = otDnsClientResolveAddress(ctx->instance,
                                            MQTT_BROKER_HOSTNAME,
                                            dns_callback, NULL,
                                            &config);
    openthread_api_mutex_unlock(ctx);

    if (err != OT_ERROR_NONE) {
        LOG_ERR("otDnsClientResolveAddress failed: %d", (int)err);
        return -1;
    }

    /* Wait for callback (up to 15s) */
    if (k_sem_take(&dns_sem, K_SECONDS(15)) != 0) {
        LOG_ERR("DNS resolution timed out");
        return -1;
    }

    return dns_result;
}

/* -------------------------------------------------------------------------- */
/* MQTT Event Handler                                                          */
/* -------------------------------------------------------------------------- */

static uint32_t mqtt_connected_uptime_ms = 0;
static uint32_t mqtt_last_pingresp_ms = 0;

/* Device MAC-based client ID — ESP32 format %04X%08X */
char device_client_id[13] = {0};

/* Forward declaration — defined later in RadioLib section */
extern SX1262 *radio;

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    uint32_t now = k_uptime_get_32();

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            mqtt_connected_uptime_ms = now;
            LOG_INF("MQTT CONNECTED to %s:%d", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);

            {
                /* Station name for topics = cfg_station (dashboard-configured).
                 * MAC (device_client_id) is only for MQTT connect and the mac JSON field. */
                extern char device_client_id[13];

                tinygs_subscribe(client, cfg_mqtt_user, cfg_station);
                tinygs_send_welcome(client, cfg_mqtt_user, cfg_station,
                                    device_client_id);

                /* Web login URL can be requested manually if needed.
                 * Publishes to tele/get_weblogin, server responds on cmnd/weblogin
                 * with a one-time URL for configuring auto-tune etc on tinygs.com.
                 * TODO: trigger via button press or config.json flag */
            }

            app_state = STATE_MQTT_CONNECTED;
        } else {
            LOG_ERR("MQTT CONNACK error: %d", evt->result);
            app_state = STATE_ERROR;
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT DISCONNECTED after %us: result=%d",
                (unsigned)(now - mqtt_connected_uptime_ms) / 1000,
                evt->result);
        app_state = STATE_ERROR;
        break;

    case MQTT_EVT_PINGRESP:
        mqtt_last_pingresp_ms = now;
        LOG_INF("MQTT PINGRESP (connected %us)",
                (unsigned)(now - mqtt_connected_uptime_ms) / 1000);
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *pub = &evt->param.publish;
        static char rx_topic[128];
        static uint8_t rx_payload[768];
        uint32_t topic_len = MIN(pub->message.topic.topic.size, sizeof(rx_topic) - 1);
        memcpy(rx_topic, pub->message.topic.topic.utf8, topic_len);
        rx_topic[topic_len] = '\0';

        uint32_t payload_len = MIN(pub->message.payload.len, sizeof(rx_payload) - 1);
        int ret = mqtt_read_publish_payload(client, rx_payload, payload_len);
        if (ret < 0) {
            LOG_ERR("MQTT payload read error: %d", ret);
            break;
        }
        rx_payload[ret] = '\0';

        LOG_INF("MQTT RX [%s] (%d bytes)", rx_topic, ret);
        if (ret < 200) {
            LOG_INF("  payload: %s", (char *)rx_payload);
        }

        /* Extract command name from topic:
         * tinygs/global/cmnd/XXX or tinygs/user/station/cmnd/XXX */
        const char *cmnd = strstr(rx_topic, "/cmnd/");
        if (cmnd) {
            cmnd += 6;  /* skip "/cmnd/" */
            LOG_INF("  command: %s", cmnd);

            if (strcmp(cmnd, "begine") == 0 ||
                strcmp(cmnd, "batch_conf") == 0) {
                /* Reconfigure radio from JSON payload.
                 * Minimal parser — extract freq, sf, bw, cr, sat */
                LOG_INF("  → Applying radio config...");
                if (radio != nullptr) {
                    const char *p;
                    /* Parse and apply frequency */
                    p = strstr((char *)rx_payload, "\"freq\":");
                    if (p) {
                        float freq = strtof(p + 7, NULL);
                        if (freq > 100.0f && freq < 1000.0f) {
                            int st = radio->setFrequency(freq);
                            tinygs_radio.frequency = freq;
                            LOG_INF("  freq=%.4f MHz (%s)", (double)freq,
                                    st == RADIOLIB_ERR_NONE ? "OK" : "FAIL");
                        }
                    }
                    /* Parse and apply SF */
                    p = strstr((char *)rx_payload, "\"sf\":");
                    if (p) {
                        int sf = atoi(p + 5);
                        if (sf >= 5 && sf <= 12) {
                            radio->setSpreadingFactor(sf);
                            tinygs_radio.sf = sf;
                            LOG_INF("  sf=%d", sf);
                        }
                    }
                    /* Parse and apply BW */
                    p = strstr((char *)rx_payload, "\"bw\":");
                    if (p) {
                        float bw = strtof(p + 5, NULL);
                        radio->setBandwidth(bw);
                        tinygs_radio.bw = bw;
                        LOG_INF("  bw=%.1f kHz", (double)bw);
                    }
                    /* Parse and apply CR */
                    p = strstr((char *)rx_payload, "\"cr\":");
                    if (p) {
                        int cr = atoi(p + 5);
                        radio->setCodingRate(cr);
                        tinygs_radio.cr = cr;
                        LOG_INF("  cr=%d", cr);
                    }
                    /* Parse and apply sync word */
                    p = strstr((char *)rx_payload, "\"sw\":");
                    if (p) {
                        int sw = atoi(p + 5);
                        radio->setSyncWord(sw);
                        LOG_INF("  sw=%d", sw);
                    }
                    /* Parse and apply preamble length */
                    p = strstr((char *)rx_payload, "\"pl\":");
                    if (p) {
                        int pl = atoi(p + 5);
                        radio->setPreambleLength(pl);
                        LOG_INF("  pl=%d", pl);
                    }
                    /* Parse and apply inverted IQ */
                    p = strstr((char *)rx_payload, "\"iIQ\":");
                    if (p) {
                        bool iiq = (strncmp(p + 6, "true", 4) == 0);
                        if (iiq) {
                            radio->invertIQ(true);
                        } else {
                            radio->invertIQ(false);
                        }
                        LOG_INF("  iIQ=%s", iiq ? "true" : "false");
                    }
                    /* Parse and apply CRC */
                    p = strstr((char *)rx_payload, "\"crc\":");
                    if (p) {
                        bool crc = (strncmp(p + 6, "true", 4) == 0);
                        radio->setCRC(crc ? 2 : 0);
                        LOG_INF("  crc=%s", crc ? "on" : "off");
                    }
                    /* Parse satellite name */
                    p = strstr((char *)rx_payload, "\"sat\":\"");
                    if (p) {
                        const char *end = strchr(p + 7, '"');
                        if (end && (end - p - 7) < (int)sizeof(tinygs_radio.satellite)) {
                            memcpy(tinygs_radio.satellite, p + 7, end - p - 7);
                            tinygs_radio.satellite[end - p - 7] = '\0';
                            LOG_INF("  satellite: %s", tinygs_radio.satellite);
                        }
                    }
                    /* Parse NORAD */
                    p = strstr((char *)rx_payload, "\"NORAD\":");
                    if (p) {
                        tinygs_radio.norad = (uint32_t)atoi(p + 8);
                        LOG_INF("  NORAD: %u", (unsigned)tinygs_radio.norad);
                    }
                    /* Don't store raw begine JSON as modem_conf — it contains
                     * unescaped quotes that break our snprintf JSON serialization.
                     * modem_conf stays as "{}" until we have proper JSON escaping.
                     * TODO: escape quotes or use a JSON library for welcome payload.
                     *
                     * Don't save radio state to NVS — satellites change every
                     * minute, which would blow out flash wear-leveling. The server
                     * sends a fresh begine within 60s of every MQTT connect. */
                    /* Restart reception */
                    radio->startReceive();
                    LOG_INF("  Radio reconfigured, listening");
                }
            } else if (strcmp(cmnd, "freq") == 0) {
                /* Direct frequency set (MHz as number) */
                float freq = strtof((char *)rx_payload, NULL);
                if (radio && freq > 100.0f && freq < 1000.0f) {
                    radio->setFrequency(freq);
                    tinygs_radio.frequency = freq;
                    radio->startReceive();
                    LOG_INF("  → freq=%.4f MHz", (double)freq);
                }
            } else if (strcmp(cmnd, "sat") == 0) {
                /* Select satellite — payload is JSON string with sat name */
                const char *p = strstr((char *)rx_payload, "\"");
                if (p) {
                    const char *end = strchr(p + 1, '"');
                    if (end && (end - p - 1) < (int)sizeof(tinygs_radio.satellite)) {
                        memcpy(tinygs_radio.satellite, p + 1, end - p - 1);
                        tinygs_radio.satellite[end - p - 1] = '\0';
                        LOG_INF("  → satellite: %s", tinygs_radio.satellite);
                    }
                }
            } else if (strcmp(cmnd, "set_pos_prm") == 0) {
                tinygs_handle_set_pos((char *)rx_payload, ret);
            } else if (strcmp(cmnd, "set_name") == 0) {
                /* Rename station: ["MAC", "new_name"]
                 * Only apply if MAC matches our device. Needs NVS to persist. */
                extern char device_client_id[13];
                const char *p = strstr((char *)rx_payload, "\"");
                if (p) {
                    const char *end = strchr(p + 1, '"');
                    if (end && (end - p - 1) == 12) {
                        char mac[13];
                        memcpy(mac, p + 1, 12);
                        mac[12] = '\0';
                        if (strcmp(mac, device_client_id) == 0) {
                            /* Find second string (new name) */
                            p = strchr(end + 1, '"');
                            if (p) {
                                end = strchr(p + 1, '"');
                                if (end) {
                                    char name[32];
                                    size_t nlen = end - p - 1;
                                    if (nlen < sizeof(name)) {
                                        memcpy(name, p + 1, nlen);
                                        name[nlen] = '\0';
                                        LOG_INF("  → Rename requested: %s (needs NVS persist)", name);
                                        /* TODO: persist to NVS, reconnect with new name */
                                    }
                                }
                            }
                        } else {
                            LOG_DBG("  → set_name: MAC mismatch (%s != %s)", mac, device_client_id);
                        }
                    }
                }
            } else if (strcmp(cmnd, "status") == 0) {
                tinygs_send_status(client, cfg_mqtt_user, cfg_station);
            } else if (strcmp(cmnd, "reset") == 0) {
                LOG_WRN("  → Reset requested by server");
                k_msleep(500);
                sys_reboot(SYS_REBOOT_COLD);
            } else if (strcmp(cmnd, "tx") == 0) {
                LOG_INF("  → TX not supported (tx=false)");
            } else if (strcmp(cmnd, "log") == 0) {
                LOG_INF("  → Server: %s", (char *)rx_payload);
            } else if (strcmp(cmnd, "sleep") == 0 || strcmp(cmnd, "siesta") == 0) {
                LOG_INF("  → Sleep requested (not implemented yet)");
            } else if (strcmp(cmnd, "foff") == 0) {
                /* Frequency offset in Hz */
                LOG_INF("  → Freq offset: %s Hz (TODO: apply)", (char *)rx_payload);
            } else if (strcmp(cmnd, "filter") == 0) {
                LOG_INF("  → Packet filter: %s (TODO: apply)", (char *)rx_payload);
            } else if (strcmp(cmnd, "update") == 0) {
                LOG_INF("  → OTA update not supported (UF2 bootloader)");
            } else if (strcmp(cmnd, "weblogin") == 0) {
                LOG_INF("  *** TinyGS Web Login URL: %s", (char *)rx_payload);
                LOG_INF("  *** Open this URL to configure auto-tune and other settings");
            } else {
                LOG_INF("  → Unhandled command: %s", cmnd);
            }
        }

        if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param ack = { .message_id = pub->message_id };
            mqtt_publish_qos1_ack(client, &ack);
        }
        break;
    }

    case MQTT_EVT_SUBACK:
        LOG_INF("MQTT SUBACK id=%u result=%d",
                evt->param.suback.message_id, evt->result);
        break;

    default:
        LOG_DBG("MQTT event: %d (connected %us)",
                evt->type,
                (unsigned)(now - mqtt_connected_uptime_ms) / 1000);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* MQTT-TLS Connection                                                         */
/* -------------------------------------------------------------------------- */

static int mqtt_tls_connect(void)
{
    LOG_INF("Connecting MQTT to [%s]:%d ...", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);

    mqtt_client_init(&mqtt_client);

    mqtt_client.broker = &broker_addr;

    /* Populate device client ID if not done yet */
    if (device_client_id[0] == '\0') {
        uint64_t dev_id = NRF_FICR->DEVICEID[0] |
                          ((uint64_t)NRF_FICR->DEVICEID[1] << 32);
        snprintf(device_client_id, sizeof(device_client_id),
                 "%04X%08X", (uint16_t)(dev_id >> 32), (uint32_t)dev_id);
        LOG_INF("Device client ID: %s", device_client_id);
    }
    mqtt_client.client_id.utf8 = (uint8_t *)device_client_id;
    mqtt_client.client_id.size = strlen(device_client_id);

    static struct mqtt_utf8 username;
    username.utf8 = (uint8_t *)cfg_mqtt_user;
    username.size = strlen(cfg_mqtt_user);
    static struct mqtt_utf8 password;
    password.utf8 = (uint8_t *)cfg_mqtt_pass;
    password.size = strlen(cfg_mqtt_pass);
    mqtt_client.user_name = &username;
    mqtt_client.password = &password;

    /* Last Will Testament — tells server we disconnected */
    static char will_topic_str[128];
    snprintf(will_topic_str, sizeof(will_topic_str),
             TINYGS_TOPIC_STAT, cfg_mqtt_user, cfg_station, TINYGS_STAT_STATUS);
    static struct mqtt_topic will_topic = {
        .qos = MQTT_QOS_1_AT_LEAST_ONCE,
    };
    will_topic.topic.utf8 = (uint8_t *)will_topic_str;
    will_topic.topic.size = strlen(will_topic_str);
    static struct mqtt_utf8 will_msg = {
        .utf8 = (uint8_t *)"0",
        .size = 1,
    };
    mqtt_client.will_topic = &will_topic;
    mqtt_client.will_message = &will_msg;

    /* Buffers */
    mqtt_client.rx_buf = mqtt_rx_buf;
    mqtt_client.rx_buf_size = sizeof(mqtt_rx_buf);
    mqtt_client.tx_buf = mqtt_tx_buf;
    mqtt_client.tx_buf_size = sizeof(mqtt_tx_buf);

    /* Event handler */
    mqtt_client.evt_cb = mqtt_evt_handler;

    /* TLS direct to mqtt.tinygs.com via nat64.net public NAT64 */
    static sec_tag_t sec_tag_list[] = { MQTT_TLS_SEC_TAG };
    static bool cred_registered = false;

    if (!cred_registered) {
        int rc = tls_credential_add(MQTT_TLS_SEC_TAG,
                                     TLS_CREDENTIAL_CA_CERTIFICATE,
                                     tinygs_ca_cert,
                                     sizeof(tinygs_ca_cert));
        if (rc == 0 || rc == -EEXIST) {
            LOG_INF("TLS CA cert registered (sec_tag %d)", MQTT_TLS_SEC_TAG);
            cred_registered = true;
        } else {
            LOG_ERR("tls_credential_add failed: %d", rc);
        }
    }

    mqtt_client.transport.type = MQTT_TRANSPORT_SECURE;

    struct mqtt_sec_config *tls_cfg = &mqtt_client.transport.tls.config;
    tls_cfg->peer_verify = TLS_PEER_VERIFY_NONE;
    tls_cfg->cipher_list = NULL;
    tls_cfg->sec_tag_list = sec_tag_list;
    tls_cfg->sec_tag_count = ARRAY_SIZE(sec_tag_list);
    tls_cfg->hostname = MQTT_BROKER_HOSTNAME;
    tls_cfg->session_cache = TLS_SESSION_CACHE_ENABLED;

    LOG_INF("Connecting with TLS...");

    /* Enable mbedTLS debug output (level 2 = state changes + info) */
    int dbg_level = 2;
    /* Note: this gets applied after socket creation, inside mqtt_connect */

    int ret = mqtt_connect(&mqtt_client);
    if (ret != 0) {
        LOG_ERR("mqtt_connect() failed: %d (errno=%d)", ret, errno);
        return ret;
    }

    LOG_INF("mqtt_connect() returned OK, waiting for CONNACK...");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* USB MSC & FATFS                                                             */
/* -------------------------------------------------------------------------- */

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &fat_fs,
};

static const char *html_content =
    "<!DOCTYPE html><html><head><title>TinyGS Configurator</title>"
    "<style>body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:20px;background:#f4f4f9;}"
    ".card{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}"
    ".row{display:flex;gap:8px;} .row input{flex:1;}"
    "button{background:#5a67d8;color:white;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;width:100%;font-size:16px;}"
    "button:hover{background:#434190;}</style>"
    "</head><body>"
    "<div class='card'>"
    "<h1>TinyGS nRF52 Setup</h1>"
    "<p>Configure your Ground Station. Save config.json to this drive, then reboot.</p>"
    "<hr>"
    "<label>Station Name:</label><input type='text' id='station' placeholder='my_station_name'>"
    "<label>MQTT Username:</label><input type='text' id='user' placeholder='From TinyGS dashboard'>"
    "<label>MQTT Password:</label><input type='password' id='pass' placeholder='From TinyGS dashboard'>"
    "<br>"
    "<label>Station Location (required for satellite assignment):</label>"
    "<div class='row'>"
    "<input type='number' step='0.0001' id='lat' placeholder='Latitude (-33.8688)'>"
    "<input type='number' step='0.0001' id='lon' placeholder='Longitude (151.2093)'>"
    "<input type='number' step='1' id='alt' placeholder='Alt (m)'>"
    "</div>"
    "<br>"
    "<button onclick='saveConfig()'>Save config.json</button>"
    "<p style='font-size:0.8em;color:#666;'>Location must be set here &mdash; the TinyGS website cannot change it.</p>"
    "</div>"
    "<script>"
    "function saveConfig() {"
    "  const config = {"
    "    station: document.getElementById('station').value,"
    "    mqtt_user: document.getElementById('user').value,"
    "    mqtt_pass: document.getElementById('pass').value,"
    "    lat: parseFloat(document.getElementById('lat').value),"
    "    lon: parseFloat(document.getElementById('lon').value),"
    "    alt: parseFloat(document.getElementById('alt').value) || 0"
    "  };"
    "  const blob = new Blob([JSON.stringify(config, null, 2)], { type: 'application/json' });"
    "  const a = document.createElement('a');"
    "  a.href = URL.createObjectURL(blob);"
    "  a.download = 'config.json';"
    "  a.click();"
    "}"
    "</script>"
    "</body></html>";

static void setup_usb_storage(void)
{
    struct fs_file_t file;
    struct fs_dirent entry;

    LOG_INF("Mounting FATFS...");

    /* Check FAT boot sector signature BEFORE mounting. FatFs will crash
     * (HardFault) on corrupted FAT data because it follows garbage pointers.
     * Reading raw flash is safe — no FatFs code involved. If invalid,
     * format through the disk driver (which manages the write cache). */
    {
        const struct device *flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
        uint8_t sig[2];
        flash_read(flash_dev, 0xE2000 + 510, sig, 2);
        if (sig[0] == 0xFF && sig[1] == 0xFF) {
            /* Erased flash — fs_mount with MOUNT_MKFS will auto-format */
            LOG_INF("FATFS: partition erased, will auto-format on mount");
        } else if (sig[0] != 0x55 || sig[1] != 0xAA) {
            /* Corrupted data — erase partition and reboot to avoid FatFs crash */
            LOG_WRN("FATFS: corrupt boot sector (0x%02X 0x%02X), erasing",
                    sig[0], sig[1]);
            flash_erase(flash_dev, 0xE2000, 0x6000);
            LOG_INF("Partition erased, rebooting...");
            k_msleep(100);
            sys_reboot(SYS_REBOOT_COLD);
        }
    }

    int res = fs_mount(&mp);
    LOG_INF("FATFS mount: %d", res);

    if (res == 0) {
        /* Write index.html if it doesn't exist */
        if (fs_stat("/NAND:/index.html", &entry) != 0) {
            fs_file_t_init(&file);
            if (fs_open(&file, "/NAND:/index.html", FS_O_CREATE | FS_O_WRITE) == 0) {
                fs_write(&file, html_content, strlen(html_content));
                fs_sync(&file);
                fs_close(&file);
            }
        }

        /* Config.json: bidirectional sync with NVS.
         *
         * NVS is authoritative (loaded later by tinygs_config_init).
         * config.json on the USB drive is a VIEW of the config:
         * - Read config.json → import values that differ from compiled defaults
         *   (this seeds NVS on first boot or when user edits the file)
         * - Write config.json from current runtime values so the drive
         *   always shows the current state for user inspection/editing
         *
         * Flow: read → parse → overwrite with current values → unmount
         */
        {
            static char cfg_buf[512];
            struct fs_file_t cfg;

            /* Read existing config.json if present */
            fs_file_t_init(&cfg);
            if (fs_open(&cfg, "/NAND:/config.json", FS_O_READ) == 0) {
                ssize_t n = fs_read(&cfg, cfg_buf, sizeof(cfg_buf) - 1);
                fs_close(&cfg);
                if (n > 0) {
                    cfg_buf[n] = '\0';
                    LOG_INF("Read config.json (%d bytes)", (int)n);

                    const char *p;
                    p = strstr(cfg_buf, "\"lat\":");
                    if (p) {
                        float v = strtof(p + 6, NULL);
                        if (v >= -90.0f && v <= 90.0f) tinygs_station_lat = v;
                    }
                    p = strstr(cfg_buf, "\"lon\":");
                    if (p) {
                        float v = strtof(p + 6, NULL);
                        if (v >= -180.0f && v <= 180.0f) tinygs_station_lon = v;
                    }
                    p = strstr(cfg_buf, "\"alt\":");
                    if (p) tinygs_station_alt = strtof(p + 6, NULL);

                    p = strstr(cfg_buf, "\"station\":\"");
                    if (p) {
                        const char *end = strchr(p + 11, '"');
                        if (end && (end - p - 11) < (int)sizeof(cfg_station)) {
                            memcpy(cfg_station, p + 11, end - p - 11);
                            cfg_station[end - p - 11] = '\0';
                        }
                    }
                    p = strstr(cfg_buf, "\"mqtt_user\":\"");
                    if (p) {
                        const char *end = strchr(p + 13, '"');
                        if (end && (end - p - 13) < (int)sizeof(cfg_mqtt_user)) {
                            memcpy(cfg_mqtt_user, p + 13, end - p - 13);
                            cfg_mqtt_user[end - p - 13] = '\0';
                        }
                    }
                    p = strstr(cfg_buf, "\"mqtt_pass\":\"");
                    if (p) {
                        const char *end = strchr(p + 13, '"');
                        if (end && (end - p - 13) < (int)sizeof(cfg_mqtt_pass)) {
                            memcpy(cfg_mqtt_pass, p + 13, end - p - 13);
                            cfg_mqtt_pass[end - p - 13] = '\0';
                        }
                    }
                }
            }

            /* Write config.json from current values — always, so the drive
             * reflects the current state for user inspection/editing */
            fs_file_t_init(&cfg);
            if (fs_open(&cfg, "/NAND:/config.json", FS_O_CREATE | FS_O_WRITE) == 0) {
                int len = snprintf(cfg_buf, sizeof(cfg_buf),
                    "{\n"
                    "  \"station\": \"%s\",\n"
                    "  \"mqtt_user\": \"%s\",\n"
                    "  \"mqtt_pass\": \"%s\",\n"
                    "  \"lat\": %.4f,\n"
                    "  \"lon\": %.4f,\n"
                    "  \"alt\": %.0f\n"
                    "}\n",
                    cfg_station, cfg_mqtt_user, cfg_mqtt_pass,
                    (double)tinygs_station_lat,
                    (double)tinygs_station_lon,
                    (double)tinygs_station_alt);
                fs_write(&cfg, cfg_buf, len);
                fs_sync(&cfg);
                fs_close(&cfg);
                LOG_INF("Wrote config.json (%d bytes)", len);
            }
        }

        /* Flush and unmount before USB MSC takes over */
        disk_access_ioctl("NAND", DISK_IOCTL_CTRL_SYNC, NULL);
        fs_unmount(&mp);
    }

    /* USB is already enabled in main() before this function is called */
}

/* -------------------------------------------------------------------------- */
/* RadioLib (SX1262)                                                           */
/* -------------------------------------------------------------------------- */

static ZephyrHal radio_hal(lora_spi.bus, (struct spi_config *)&lora_spi.config);
static Module *radio_mod = nullptr;
SX1262 *radio = nullptr;  /* non-static — accessed from MQTT callback */

/* LoRa packet reception flag — set by DIO1 ISR */
static volatile bool lora_packet_received = false;

static void lora_rx_callback(void)
{
    lora_packet_received = true;
}

static void init_radio(void)
{
    LOG_INF("Initializing SX1262...");

    uint32_t cs   = radio_hal.addPin(&lora_cs);
    uint32_t rst  = radio_hal.addPin(&lora_reset);
    uint32_t busy = radio_hal.addPin(&lora_busy);
    uint32_t dio1 = radio_hal.addPin(&lora_dio1);

    /* Run HAL smoke tests before radio init */
    extern int zephyr_hal_run_tests(ZephyrHal *hal);
    zephyr_hal_run_tests(&radio_hal);

    radio_mod = new Module(&radio_hal, cs, dio1, rst, busy);
    radio = new SX1262(radio_mod);

    int state = radio->begin();
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("SX1262 OK");
    } else {
        LOG_ERR("SX1262 init failed: %d", state);
        return;
    }

    /* Set default LoRa config — 433MHz, SF10, BW125, CR5
     * This will be overridden by server batch_conf commands */
    state = radio->setFrequency(436.703);
    if (state != RADIOLIB_ERR_NONE) {
        LOG_ERR("setFrequency failed: %d", state);
    }
    radio->setSpreadingFactor(10);
    radio->setBandwidth(250.0);
    radio->setCodingRate(5);
    radio->setSyncWord(0x12);

    /* Register DIO1 interrupt callback and start receiving */
    radio->setPacketReceivedAction(lora_rx_callback);
    state = radio->startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("SX1262 listening on 436.703 MHz, SF10, BW250");
    } else {
        LOG_ERR("startReceive failed: %d", state);
    }
}

/*
 * Check for received LoRa packets and process them.
 * Called from the main loop. Returns true if a packet was processed.
 */
static bool lora_check_rx(void)
{
    if (!lora_packet_received || radio == nullptr) {
        return false;
    }

    lora_packet_received = false;

    size_t len = radio->getPacketLength();
    if (len == 0 || len > 255) {
        radio->startReceive();
        return false;
    }

    uint8_t data[256];
    int state = radio->readData(data, len);

    float rssi = radio->getRSSI();
    float snr = radio->getSNR();
    float freq_err = radio->getFrequencyError();

    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("LoRa RX: %u bytes, RSSI=%.1f, SNR=%.1f, FreqErr=%.1f",
                (unsigned)len, (double)rssi, (double)snr, (double)freq_err);
        LOG_HEXDUMP_INF(data, MIN(len, 32), "Packet data:");

        /* Publish via MQTT if connected */
        if (app_state == STATE_MQTT_CONNECTED) {
            tinygs_send_rx(&mqtt_client, cfg_mqtt_user, cfg_station,
                           data, len, rssi, snr, freq_err);
        }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        LOG_WRN("LoRa RX: CRC error, %u bytes", (unsigned)len);
    } else {
        LOG_ERR("LoRa readData failed: %d", state);
    }

    /* Restart reception */
    radio->startReceive();
    return true;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */

int main(void)
{
    const struct device *const console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;

    if (device_is_ready(console_dev)) {
        cdc_acm_dte_rate_callback_set(console_dev, baudrate_reset_handler);
    }

    enable_peripherals();

    /* FATFS operations BEFORE USB enable — mount, write, read, unmount.
     * Must complete before USB MSC goes live to avoid concurrent access. */
    setup_usb_storage();

    /* Enable USB composite (CDC ACM console + MSC drive) */
    if (usb_enable(NULL) == 0) {
        LOG_INF("USB active (MSC + CDC ACM)");
    }

    /* Wait briefly for serial monitor to connect */
    for (int i = 0; i < 30 && !dtr; i++) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_msleep(100);
    }

    LOG_INF("=== TinyGS nRF52 Phase 2: MQTT-TLS over Thread ===");
    log_heap_usage("boot");
    init_openthread();

    /* Load persistent config from NVS. Must be AFTER init_openthread()
     * because OpenThread initializes the settings/NVS subsystem.
     * No extra CONFIG_SETTINGS needed in prj.conf — OpenThread provides it.
     * config.json values are loaded first (in setup_usb_storage), then
     * NVS overrides with any previously saved values. */
    tinygs_config_init();

    init_radio();

    int retry_count = 0;
    int mqtt_poll_fd_count = 0;
    struct zsock_pollfd mqtt_poll_fd;

    while (1) {
        switch (app_state) {

        case STATE_WAIT_THREAD:
            if (thread_attached) {
                log_heap_usage("thread_attached");
                LOG_INF("--- Thread attached, waiting 5s for routing to stabilize ---");
                k_msleep(5000);
                app_state = STATE_DNS_RESOLVE;
                retry_count = 0;
            } else {
                LOG_INF("Waiting for Thread network... (%ds)", retry_count * 10);
                log_ot_diagnostics();
                retry_count++;
                k_msleep(10000);
            }
            break;

        case STATE_DNS_RESOLVE:
            log_heap_usage("pre_dns");
            log_ot_diagnostics();
            if (resolve_broker() == 0) {
                app_state = STATE_MQTT_CONNECT;
            } else {
                retry_count++;
                if (retry_count > 10) {
                    LOG_ERR("DNS failed after %d attempts", retry_count);
                    app_state = STATE_ERROR;
                } else {
                    LOG_WRN("DNS retry %d/10 in 5s...", retry_count);
                    k_msleep(5000);
                }
            }
            break;

        case STATE_MQTT_CONNECT:
            log_heap_usage("pre_tls");
            if (mqtt_tls_connect() == 0) {
                /* Set up poll fd for mqtt_input */
                mqtt_poll_fd.fd = mqtt_client.transport.tls.sock;
                mqtt_poll_fd.events = ZSOCK_POLLIN;
                mqtt_poll_fd_count = 1;

                /* Process until CONNACK or timeout */
                int wait_ms = 15000; /* 15s for TLS handshake + CONNACK */
                while (app_state == STATE_MQTT_CONNECT && wait_ms > 0) {
                    int rc = zsock_poll(&mqtt_poll_fd, mqtt_poll_fd_count, 1000);
                    if (rc > 0) {
                        mqtt_input(&mqtt_client);
                    }
                    mqtt_live(&mqtt_client);
                    wait_ms -= 1000;
                }
                if (app_state == STATE_MQTT_CONNECT) {
                    LOG_ERR("MQTT connect timeout (no CONNACK in 15s)");
                    mqtt_disconnect(&mqtt_client);
                    app_state = STATE_ERROR;
                }
            } else {
                app_state = STATE_ERROR;
            }
            break;

        case STATE_MQTT_CONNECTED: {
            log_heap_usage("mqtt_connected");
            LOG_INF("=== MQTT-TLS CONNECTION SUCCESSFUL ===");

            /* TinyGS main loop — process MQTT, LoRa RX, and pings */
            uint32_t last_ping_ms = k_uptime_get_32();

            while (app_state == STATE_MQTT_CONNECTED) {
                /* Poll MQTT socket for incoming data */
                int rc = zsock_poll(&mqtt_poll_fd, mqtt_poll_fd_count, 100);
                if (rc > 0) {
                    mqtt_input(&mqtt_client);
                }
                mqtt_live(&mqtt_client);

                /* Check for LoRa packet reception */
                lora_check_rx();

                /* Send TinyGS ping every 60s */
                uint32_t now_ms = k_uptime_get_32();
                if ((now_ms - last_ping_ms) >= (TINYGS_PING_INTERVAL_S * 1000)) {
                    tinygs_send_ping(&mqtt_client, cfg_mqtt_user, cfg_station);
                    last_ping_ms = now_ms;
                }

                k_msleep(100);  /* 100ms loop — responsive to LoRa packets */
            }
            break;
        }

        case STATE_ERROR:
            LOG_ERR("Error state. Retrying in 30s...");
            k_msleep(30000);
            app_state = thread_attached ? STATE_DNS_RESOLVE : STATE_WAIT_THREAD;
            retry_count = 0;
            break;
        }
    }

    return 0;
}
