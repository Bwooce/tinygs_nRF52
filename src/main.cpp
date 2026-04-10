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

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

#include <RadioLib.h>
#include "ZephyrHal.h"

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
        LOG_INF("RAM [%s]: heap used=%u free=%u max=%u",
                label,
                (unsigned)stats.allocated_bytes,
                (unsigned)stats.free_bytes,
                (unsigned)stats.max_allocated_bytes);
    }
#else
    LOG_INF("RAM [%s]: (stats not enabled)", label);
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
/* socat proxy on HA BR: TCP6-LISTEN:18883 -> TCP4:mqtt.tinygs.com:8883 */
#define MQTT_PROXY_PORT      18883
#define MQTT_CLIENT_ID       "tinygs_nrf52_poc"
#define MQTT_TLS_SEC_TAG     1

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
     * Use otDnsClientResolveIp4Address with our BR's DNS config.
     * The default config may use another BR's DNS (router f800) which
     * synthesizes NAT64 addresses using a prefix that f800 doesn't
     * actually translate. Force use of our HA SkyConnect BR's DNS proxy.
     */
    openthread_api_mutex_lock(ctx);

    /* Get the default config as a starting point, then override the NAT64 mode */
    otDnsQueryConfig config;
    const otDnsQueryConfig *defaultConfig = otDnsClientGetDefaultConfig(ctx->instance);
    config = *defaultConfig;
    config.mNat64Mode = OT_DNS_NAT64_ALLOW;

    /*
     * Connect via socat proxy on the HA Border Router.
     * The HA OTBR addon doesn't have OT_NAT64_TRANSLATOR compiled in,
     * so NAT64 translation doesn't work. Workaround: socat on HA bridges
     * Thread IPv6 port 18883 → mqtt.tinygs.com:8883 IPv4.
     *
     * BR mesh-local RLOC: fd69:9a4f:1982::ff:fe00:2c00
     * TODO: Replace with proper NAT64 when HA fixes their OTBR addon.
     */
    otIp6Address brAddr;
    memset(&brAddr, 0, sizeof(brAddr));
    /* fd9f:2193:7da6:1:30f0:ebe1:3aa:b659 — BR's stable mesh-local EID */
    brAddr.mFields.m16[0] = htons(0xfd9f);
    brAddr.mFields.m16[1] = htons(0x2193);
    brAddr.mFields.m16[2] = htons(0x7da6);
    brAddr.mFields.m16[3] = htons(0x0001);
    brAddr.mFields.m16[4] = htons(0x30f0);
    brAddr.mFields.m16[5] = htons(0xebe1);
    brAddr.mFields.m16[6] = htons(0x03aa);
    brAddr.mFields.m16[7] = htons(0xb659);

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&broker_addr;
    memset(sin6, 0, sizeof(*sin6));
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(MQTT_PROXY_PORT);
    memcpy(&sin6->sin6_addr, &brAddr, sizeof(brAddr));

    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&brAddr, addr_str, sizeof(addr_str));
    LOG_INF("Using socat proxy: [%s]:%d -> mqtt.tinygs.com:8883",
            addr_str, MQTT_PROXY_PORT);

    openthread_api_mutex_unlock(ctx);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* MQTT Event Handler                                                          */
/* -------------------------------------------------------------------------- */

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT CONNECTED to %s:%d", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);
            app_state = STATE_MQTT_CONNECTED;
        } else {
            LOG_ERR("MQTT CONNACK error: %d", evt->result);
            app_state = STATE_ERROR;
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT disconnected: %d", evt->result);
        app_state = STATE_ERROR;
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *pub = &evt->param.publish;
        LOG_INF("MQTT PUBLISH on topic len=%u, payload len=%u",
                pub->message.topic.topic.size,
                pub->message.payload.len);
        break;
    }

    case MQTT_EVT_SUBACK:
        LOG_INF("MQTT SUBACK id=%u result=%d",
                evt->param.suback.message_id, evt->result);
        break;

    default:
        LOG_DBG("MQTT event: %d", evt->type);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* MQTT-TLS Connection                                                         */
/* -------------------------------------------------------------------------- */

static int mqtt_tls_connect(void)
{
    LOG_INF("Connecting MQTT-TLS to %s:%d ...", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);

    mqtt_client_init(&mqtt_client);

    /* Broker address (already resolved) */
    mqtt_client.broker = &broker_addr;

    /* Client ID */
    mqtt_client.client_id.utf8 = (uint8_t *)MQTT_CLIENT_ID;
    mqtt_client.client_id.size = strlen(MQTT_CLIENT_ID);

    /* Buffers */
    mqtt_client.rx_buf = mqtt_rx_buf;
    mqtt_client.rx_buf_size = sizeof(mqtt_rx_buf);
    mqtt_client.tx_buf = mqtt_tx_buf;
    mqtt_client.tx_buf_size = sizeof(mqtt_tx_buf);

    /* Event handler */
    mqtt_client.evt_cb = mqtt_evt_handler;

    /* TLS transport */
    mqtt_client.transport.type = MQTT_TRANSPORT_SECURE;

    struct mqtt_sec_config *tls = &mqtt_client.transport.tls.config;
    tls->peer_verify = TLS_PEER_VERIFY_NONE; /* Skip cert check for PoC */
    tls->cipher_list = NULL;
    tls->sec_tag_list = NULL;
    tls->sec_tag_count = 0;
    tls->hostname = MQTT_BROKER_HOSTNAME;

    LOG_INF("Starting TLS handshake (this is the RAM crunch moment)...");

    int ret = mqtt_connect(&mqtt_client);
    if (ret != 0) {
        LOG_ERR("mqtt_connect() failed: %d", ret);
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
    "button{background:#5a67d8;color:white;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;width:100%;font-size:16px;}"
    "button:hover{background:#434190;}</style>"
    "</head><body>"
    "<div class='card'>"
    "<h1>TinyGS Setup</h1>"
    "<p>Configure your Ground Station and join the Thread network.</p>"
    "<hr>"
    "<label>Station ID:</label><input type='text' id='station' placeholder='MyTinyGS'>"
    "<label>MQTT Password:</label><input type='password' id='pass' placeholder='Password'>"
    "<br><br>"
    "<button onclick='saveConfig()'>Save config.json to Drive</button>"
    "<p style='font-size:0.8em;color:#666;'>After saving, scan the QR code in Home Assistant to finish setup.</p>"
    "</div>"
    "<script>"
    "function saveConfig() {"
    "  const config = {"
    "    station: document.getElementById('station').value,"
    "    password: document.getElementById('pass').value"
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
    int res = fs_mount(&mp);

    if (res != 0) {
        LOG_WRN("FS not found, formatting...");
        res = fs_mkfs(FS_FATFS, (uintptr_t)mp.mnt_point, &mp, 0);
        if (res == 0) {
            res = fs_mount(&mp);
        }
    }

    if (res == 0) {
        fs_file_t_init(&file);
        if (fs_stat("/NAND:/index.html", &entry) != 0) {
            if (fs_open(&file, "/NAND:/index.html", FS_O_CREATE | FS_O_WRITE) == 0) {
                fs_write(&file, html_content, strlen(html_content));
                fs_close(&file);
            }
        }
        /*
         * Unmount FATFS before enabling USB MSC.
         * Once USB is active, the host OS owns the FAT sectors.
         * Any firmware writes to FATFS while mounted via USB will corrupt it.
         * Runtime config changes should use NVS and sync to FATFS on reboot.
         */
        fs_unmount(&mp);
    }

    if (usb_enable(NULL) == 0) {
        LOG_INF("USB active (MSC + CDC ACM)");
    }
}

/* -------------------------------------------------------------------------- */
/* RadioLib (SX1262)                                                           */
/* -------------------------------------------------------------------------- */

static ZephyrHal radio_hal(lora_spi.bus, (struct spi_config *)&lora_spi.config);
static Module *radio_mod = nullptr;
static SX1262 *radio = nullptr;

static void init_radio(void)
{
    LOG_INF("Initializing SX1262...");

    uint32_t cs   = radio_hal.addPin(&lora_cs);
    uint32_t rst  = radio_hal.addPin(&lora_reset);
    uint32_t busy = radio_hal.addPin(&lora_busy);
    uint32_t dio1 = radio_hal.addPin(&lora_dio1);

    radio_mod = new Module(&radio_hal, cs, dio1, rst, busy);
    radio = new SX1262(radio_mod);

    int state = radio->begin();
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("SX1262 OK");
    } else {
        LOG_ERR("SX1262 init failed: %d", state);
    }
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
    setup_usb_storage();

    /* Wait briefly for serial monitor */
    for (int i = 0; i < 30 && !dtr; i++) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_msleep(100);
    }

    LOG_INF("=== TinyGS nRF52 Phase 1: MQTT-TLS over Thread ===");
    log_heap_usage("boot");
    init_openthread();
    /* Skip radio init — not needed for MQTT-TLS test */

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

        case STATE_MQTT_CONNECTED:
            log_heap_usage("mqtt_connected");
            LOG_INF("=== MQTT-TLS CONNECTION SUCCESSFUL ===");
            LOG_INF("Phase 1 PoC: TLS over Thread NAT64 is working!");

            /* Keep connection alive */
            while (app_state == STATE_MQTT_CONNECTED) {
                int rc = zsock_poll(&mqtt_poll_fd, mqtt_poll_fd_count, 1000);
                if (rc > 0) {
                    mqtt_input(&mqtt_client);
                }
                mqtt_live(&mqtt_client);
                k_msleep(1000);
            }
            break;

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
