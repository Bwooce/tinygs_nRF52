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
#include <zephyr/sys/base64.h>
#include <zephyr/drivers/watchdog.h>
#if defined(CONFIG_LED_STRIP)
#include <zephyr/drivers/led_strip.h>
#endif
#include <openthread/sntp.h>
#include <time.h>
#include "tinygs_display.h"
#include <zephyr/drivers/hwinfo.h>
#include <hal/nrf_power.h>
#include <hal/nrf_ficr.h>

#include <mbedtls/debug.h>
#include <RadioLib.h>
#include "hal/Zephyr/ZephyrHal.h"
#include "tinygs_protocol.h"
#include "tinygs_json.h"
#include "tinygs_config.h"

LOG_MODULE_REGISTER(tinygs_nrf52, LOG_LEVEL_DBG);

/* Crash diagnostic — __noinit survives warm reset (SREQ doesn't clear RAM) */
#define CRASH_MAGIC 0xDEAD0000
static volatile uint32_t __noinit crash_reason;
static volatile uint32_t __noinit crash_pc;
static volatile uint32_t __noinit crash_lr;

extern "C" void k_sys_fatal_error_handler(unsigned int reason,
                                           const z_arch_esf_t *esf)
{
    crash_reason = CRASH_MAGIC | (reason & 0xFFFF);
    if (esf) {
        crash_pc = esf->basic.pc;
        crash_lr = esf->basic.lr;
    }
    sys_reboot(SYS_REBOOT_COLD);
}

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

enum app_state app_state = STATE_WAIT_THREAD;
volatile bool thread_attached = false;

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
static uint8_t mqtt_rx_buf[256];  /* Streaming read: headers + topic of incoming PUBLISH */
static uint8_t mqtt_tx_buf[128];  /* MQTT framing only: header+topic; payload sent via iovec */
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
/* Hardware Watchdog                                                            */
/* -------------------------------------------------------------------------- */

static const struct device *wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel_id = -1;

static void watchdog_init(void)
{
    if (!device_is_ready(wdt_dev)) {
        LOG_WRN("Watchdog not available");
        return;
    }

    struct wdt_timeout_cfg cfg = {
        .window = {
            .min = 0,
            .max = CONFIG_MQTT_KEEPALIVE * 2 * 1000, /* 2x keepalive in ms */
        },
        .callback = NULL, /* reset on timeout */
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt_dev, &cfg);
    if (wdt_channel_id < 0) {
        LOG_ERR("Watchdog install failed: %d", wdt_channel_id);
        return;
    }

    int ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        LOG_ERR("Watchdog setup failed: %d", ret);
        return;
    }

    LOG_INF("Watchdog: %ds timeout (2x keepalive)", CONFIG_MQTT_KEEPALIVE * 2);
}

static void watchdog_feed(void)
{
    if (wdt_channel_id >= 0) {
        wdt_feed(wdt_dev, wdt_channel_id);
    }
}

/* -------------------------------------------------------------------------- */
/* Status LED (green, P1.03, active low)                                       */
/* -------------------------------------------------------------------------- */

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

#if defined(CONFIG_LED_STRIP)
static const struct device *led_strip = DEVICE_DT_GET_OR_NULL(DT_ALIAS(led_strip));
static struct led_rgb leds[2] = {{0}, {0}};
#else
static const void *led_strip = NULL;
#endif

static void led_init(void)
{
    if (device_is_ready(status_led.port)) {
        gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
    }
#if defined(CONFIG_LED_STRIP)
    if (led_strip && device_is_ready(led_strip)) {
        /* Clear NeoPixels immediately — bootloader may have left them lit */
        struct led_rgb off[2] = {{0}, {0}};
        led_strip_update_rgb(led_strip, off, 2);
        LOG_INF("NeoPixel: 2x SK6812/WS2812 cleared");
    } else {
        led_strip = NULL;
    }
#endif
}

/* -------------------------------------------------------------------------- */
/* Breathing LED — nRF PWM hardware sequence, zero CPU                         */
/* -------------------------------------------------------------------------- */

#include <nrfx_pwm.h>
#include <hal/nrf_gpio.h>

static nrfx_pwm_t pwm_led = NRFX_PWM_INSTANCE(0);
static bool pwm_breathing = false;

/* Sine-ish breathing curve: 64 steps, ramps up then down smoothly.
 * Values are inverted (LED active LOW): 1000=off, 0=full brightness.
 * PWM top value = 1000 for ~1kHz at 1MHz base clock. */
static nrf_pwm_values_individual_t breath_values[64];
static nrf_pwm_sequence_t breath_seq = {
    .values = { .p_individual = breath_values },
    .length = NRF_PWM_VALUES_LENGTH(breath_values),
    .repeats = 30,   /* Hold each step for 30 PWM periods (~30ms at 1kHz) = ~2s full cycle */
    .end_delay = 0,
};

static void breathing_led_init(void)
{
    /* Build sine breathing table (active LOW: 1000=off, 0=bright) */
    for (int i = 0; i < 64; i++) {
        /* Triangle wave: 0→31 ramp up, 32→63 ramp down */
        int brightness = (i < 32) ? i : (63 - i);
        /* Square for softer ramp, scale to 0-1000 */
        int duty = 1000 - (brightness * brightness * 1000 / (31 * 31));
        breath_values[i].channel_0 = (uint16_t)duty;
        breath_values[i].channel_1 = 1000; /* unused channels off */
        breath_values[i].channel_2 = 1000;
        breath_values[i].channel_3 = 1000;
    }

    nrfx_pwm_config_t config = {
        .output_pins = {
            NRF_GPIO_PIN_MAP(1, 3),  /* P1.03 = green LED */
            NRF_PWM_PIN_NOT_CONNECTED,
            NRF_PWM_PIN_NOT_CONNECTED,
            NRF_PWM_PIN_NOT_CONNECTED,
        },
        .pin_inverted = { true, false, false, false }, /* Active LOW LED */
        .irq_priority = NRFX_PWM_DEFAULT_CONFIG_IRQ_PRIORITY,
        .base_clock   = NRF_PWM_CLK_1MHz,
        .count_mode   = NRF_PWM_MODE_UP,
        .top_value    = 1000,
        .load_mode    = NRF_PWM_LOAD_INDIVIDUAL,
        .step_mode    = NRF_PWM_STEP_AUTO,
    };

    if (nrfx_pwm_init(&pwm_led, &config, NULL, NULL) == NRFX_SUCCESS) {
        LOG_INF("Breathing LED: PWM0 on P1.03");
    }
}

static void breathing_led_start(void)
{
    if (!pwm_breathing) {
        nrfx_pwm_simple_playback(&pwm_led, &breath_seq, 0, NRFX_PWM_FLAG_LOOP);
        pwm_breathing = true;
    }
}

static void breathing_led_stop(void)
{
    if (pwm_breathing) {
        nrfx_pwm_stop(&pwm_led, false);
        pwm_breathing = false;
        /* Ensure LED is off (active LOW: drive HIGH to turn off) */
        nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1, 3));
    }
}

static void led_set(bool on)
{
    if (device_is_ready(status_led.port)) {
        gpio_pin_set_dt(&status_led, on ? 1 : 0);
    }
}

/* NeoPixel colors — LED0=status (problems only), LED1=activity */
#define NEO_OFF       0, 0, 0
#define NEO_RED       20, 0, 0
#define NEO_GREEN     0, 10, 0
#define NEO_BLUE      0, 0, 20
#define NEO_YELLOW    15, 10, 0
#define NEO_WHITE     30, 30, 30

/* NeoPixel functions — no-ops when LED_STRIP is disabled */
static void neopixel_set(uint8_t r0, uint8_t g0, uint8_t b0,
                          uint8_t r1, uint8_t g1, uint8_t b1)
{
#if defined(CONFIG_LED_STRIP)
    if (!led_strip) return;
    leds[0].r = r0; leds[0].g = g0; leds[0].b = b0;
    leds[1].r = r1; leds[1].g = g1; leds[1].b = b1;
    led_strip_update_rgb(led_strip, leds, 2);
#endif
    (void)r0; (void)g0; (void)b0; (void)r1; (void)g1; (void)b1;
}

static void neopixel_off(void)
{
    neopixel_set(0, 0, 0, 0, 0, 0);
}

/* -------------------------------------------------------------------------- */
/* Battery Voltage ADC                                                         */
/* -------------------------------------------------------------------------- */

/* Battery ADC: P0.04 (AIN2), bias enable from DTS alias.
 * Voltage divider 100k:390k → multiplier ~4.9 */
static const struct gpio_dt_spec adc_ctrl = GPIO_DT_SPEC_GET(DT_ALIAS(adc_ctrl), gpios);

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
static uint32_t mqtt_rx_count = 0;  /* MQTT messages received */
static uint32_t lora_rx_count = 0;  /* LoRa packets received */
static bool mqtt_first_connect = true; /* Clean session only on first connect */
static uint32_t doppler_interval_ms = 4000; /* Updated by foff [offset, tol, refresh] */
static uint32_t mqtt_last_pingresp_ms = 0;

/* Device MAC-based client ID — ESP32 format %04X%08X */
char device_client_id[13] = {0};

/* Forward declaration — defined later in RadioLib section */
/* Radio pointer type matches DTS compatible */
#if DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1262)
extern SX1262 *radio;
#elif DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1268)
extern SX1268 *radio;
#elif DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1276)
extern SX1276 *radio;
#elif DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1278)
extern SX1278 *radio;
#endif

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    uint32_t now = k_uptime_get_32();

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            mqtt_connected_uptime_ms = now;
            watchdog_feed(); /* connection alive */
            LOG_INF("MQTT CONNECTED to %s:%d", MQTT_BROKER_HOSTNAME, MQTT_BROKER_PORT);

            {
                /* Station name for topics = cfg_station (dashboard-configured).
                 * MAC (device_client_id) is only for MQTT connect and the mac JSON field. */
                extern char device_client_id[13];

                tinygs_subscribe(client, cfg_mqtt_user, cfg_station);
                tinygs_send_welcome(client, cfg_mqtt_user, cfg_station,
                                    device_client_id);

                /* Web login URL: press BOOT button while display is active
                 * to publish tele/get_weblogin. Server responds on cmnd/weblogin
                 * with a one-time URL for configuring auto-tune etc. */
            }

            mqtt_first_connect = false; /* Next reconnect uses clean_session=0 */
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
        watchdog_feed();
        LOG_INF("MQTT PINGRESP (connected %us)",
                (unsigned)(now - mqtt_connected_uptime_ms) / 1000);
        break;

    case MQTT_EVT_PUBLISH: {
        mqtt_rx_count++;
        watchdog_feed(); /* Any RX proves connection is alive */
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
        /* Truncate long payloads for logging — full payload is still parsed */
        if (ret > 0) {
            char saved = 0;
            if (ret > 200) { saved = rx_payload[200]; rx_payload[200] = '\0'; }
            LOG_INF("  payload: %s%s", (char *)rx_payload, ret > 200 ? "..." : "");
            if (saved) { rx_payload[200] = saved; }
        }

        /* Extract command name from topic:
         * tinygs/global/cmnd/XXX or tinygs/user/station/cmnd/XXX */
        const char *cmnd = strstr(rx_topic, "/cmnd/");
        if (cmnd) {
            cmnd += 6;  /* skip "/cmnd/" */
            LOG_INF("  command: %s", cmnd);

            if (strcmp(cmnd, "begine") == 0 ||
                strcmp(cmnd, "batch_conf") == 0) {
                if (radio != nullptr) {
                    /* Store raw payload as modem_conf before parsing
                     * (json_obj_parse modifies the buffer in-place) */
                    size_t conf_len = strlen((char *)rx_payload);
                    if (conf_len < sizeof(tinygs_radio.modem_conf)) {
                        memcpy(tinygs_radio.modem_conf, rx_payload, conf_len + 1);
                    } else {
                        LOG_WRN("modem_conf too large (%zu > %zu), skipped",
                                conf_len, sizeof(tinygs_radio.modem_conf) - 1);
                    }

                    /* Parse begine JSON via Zephyr json.h descriptors */
                    struct tinygs_begine_msg msg;
                    int64_t parsed = tinygs_parse_begine((char *)rx_payload, ret, &msg);
                    if (parsed < 0) {
                        LOG_ERR("begine parse failed");
                    } else {
                        /* Apply radio config from parsed struct */
                        float freq = tinygs_begine_get_freq(&msg);
                        if (freq > 100.0f && freq < 1000.0f) {
                            tinygs_radio.frequency = freq;
                            radio->setFrequency(freq + tinygs_radio.freq_offset / 1e6f);
                        }
                        float bw = tinygs_begine_get_bw(&msg);
                        if (bw > 0.0f) {
                            radio->setBandwidth(bw);
                            tinygs_radio.bw = bw;
                        }
                        if (msg.sf >= 5 && msg.sf <= 12) {
                            radio->setSpreadingFactor(msg.sf);
                            tinygs_radio.sf = msg.sf;
                        }
                        if (msg.cr >= 4 && msg.cr <= 8) {
                            radio->setCodingRate(msg.cr);
                            tinygs_radio.cr = msg.cr;
                        }
                        radio->setSyncWord(msg.sw);
                        radio->setPreambleLength(msg.pl);
                        radio->invertIQ(msg.iIQ);
                        radio->setCRC(msg.crc ? 2 : 0);

                        /* FLDRO — forced Low Data Rate Optimization (CRITICAL for reception) */
                        if (msg.fldro == 2) {
                            radio->autoLDRO();
                        } else {
                            radio->forceLDRO(msg.fldro ? true : false);
                        }

                        /* Implicit/explicit header — cl>0 means implicit with fixed length */
                        if (msg.cl > 0) {
                            radio->implicitHeader(msg.cl);
                        } else {
                            radio->explicitHeader();
                        }

                        /* RX boosted gain — ~3dB better sensitivity on SX1262 */
                        radio->setRxBoostedGainMode(true);

                        if (msg.sat) {
                            strncpy(tinygs_radio.satellite, msg.sat,
                                    sizeof(tinygs_radio.satellite) - 1);
                            tinygs_radio.satellite[sizeof(tinygs_radio.satellite) - 1] = '\0';
                        }
                        tinygs_radio.norad = (uint32_t)msg.NORAD;
                    }

                    /* Parse filter (array — not handled by json.h descriptors) */
                    int filt_count = tinygs_parse_filter(
                        tinygs_radio.modem_conf, strlen(tinygs_radio.modem_conf),
                        tinygs_radio.filter, sizeof(tinygs_radio.filter));
                    tinygs_radio.filter_len = (filt_count > 0) ? filt_count : 0;
                    if (filt_count <= 0) tinygs_radio.filter[0] = 0;

                    /* Parse TLE base64 from modem_conf copy (original modified by json_obj_parse) */
                    const char *tlx_p = strstr(tinygs_radio.modem_conf, "\"tlx\":\"");
                    if (tlx_p) {
                        const char *b64_start = tlx_p + 7;
                        const char *b64_end = strchr(b64_start, '"');
                        if (b64_end) {
                            size_t decoded_len = 0;
                            if (base64_decode((uint8_t *)tinygs_radio.tle,
                                              sizeof(tinygs_radio.tle),
                                              &decoded_len,
                                              (const uint8_t *)b64_start,
                                              b64_end - b64_start) == 0
                                && decoded_len == 34) {
                                tinygs_radio.tle_valid = true;
                                tinygs_radio.freq_doppler = 0.0f;
                                LOG_INF("  TLE received (%zu bytes)", decoded_len);
                            } else {
                                tinygs_radio.tle_valid = false;
                            }
                        }
                    } else {
                        tinygs_radio.tle_valid = false;
                    }

                    radio->startReceive();

                    LOG_INF("  → %s %.4fMHz SF%d BW%.1f%s%s",
                            tinygs_radio.satellite,
                            (double)tinygs_radio.frequency,
                            tinygs_radio.sf,
                            (double)tinygs_radio.bw,
                            tinygs_radio.tle_valid ? " TLE" : "",
                            tinygs_radio.filter[0] ? " FLT" : "");
                }
            } else if (strcmp(cmnd, "freq") == 0) {
                /* Direct frequency set (MHz as number) */
                float freq = strtof((char *)rx_payload, NULL);
                if (radio && freq > 100.0f && freq < 1000.0f) {
                    tinygs_radio.frequency = freq;
                    radio->setFrequency(freq + tinygs_radio.freq_offset / 1e6f);
                    radio->startReceive();
                    LOG_INF("  → freq=%.4f MHz (foff=%.0f Hz)",
                            (double)freq, (double)tinygs_radio.freq_offset);
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
                extern char device_client_id[13];
                struct tinygs_name_msg name_msg;
                if (tinygs_parse_set_name((char *)rx_payload, ret, &name_msg) == 0) {
                    if (strcmp(name_msg.mac, device_client_id) == 0) {
                        strncpy(cfg_station, name_msg.name, sizeof(cfg_station) - 1);
                        cfg_station[sizeof(cfg_station) - 1] = '\0';
                        tinygs_config_save_station(cfg_station);
                        LOG_INF("  → Renamed to: %s (saved, rebooting)", cfg_station);
                        k_msleep(500);
                        sys_reboot(SYS_REBOOT_COLD);
                    } else {
                        LOG_DBG("  → set_name: MAC mismatch");
                    }
                }
            } else if (strcmp(cmnd, "status") == 0) {
                tinygs_send_status(client, cfg_mqtt_user, cfg_station);
            } else if (strcmp(cmnd, "reset") == 0) {
                LOG_WRN("*** SERVER RESET — rebooting ***");
                mqtt_disconnect(client);
                k_msleep(2000);
                sys_reboot(SYS_REBOOT_COLD);
            } else if (strcmp(cmnd, "tx") == 0) {
                LOG_INF("  → TX not supported (tx=false)");
            } else if (strcmp(cmnd, "log") == 0) {
                LOG_INF("  → Server: %s", (char *)rx_payload);
            } else if (strcmp(cmnd, "sleep") == 0 || strcmp(cmnd, "siesta") == 0) {
                LOG_INF("  → Sleep not implemented (Phase 4 power management)");
            } else if (strcmp(cmnd, "foff") == 0) {
                float foff = tinygs_parse_foff((char *)rx_payload, ret,
                                                &tinygs_radio.doppler_tol,
                                                &doppler_interval_ms);
                tinygs_radio.freq_offset = foff;
                if (radio) {
                    radio->setFrequency(tinygs_radio.frequency +
                                        tinygs_radio.freq_offset / 1e6f);
                    radio->startReceive();
                }
                LOG_INF("  → foff=%.0f Hz tol=%.0f Hz refresh=%ums",
                        (double)foff, (double)tinygs_radio.doppler_tol,
                        (unsigned)doppler_interval_ms);
            } else if (strcmp(cmnd, "filter") == 0) {
                int fcount = tinygs_parse_filter((char *)rx_payload, ret,
                                                  tinygs_radio.filter,
                                                  sizeof(tinygs_radio.filter));
                tinygs_radio.filter_len = (fcount > 0) ? fcount : 0;
                LOG_INF("  → filter: %d bytes", tinygs_radio.filter_len);
            } else if (strcmp(cmnd, "update") == 0) {
                LOG_INF("  → OTA update not supported (UF2 bootloader)");
            } else if (strcmp(cmnd, "weblogin") == 0) {
                LOG_INF("  *** TinyGS Web Login URL: %s", (char *)rx_payload);
                LOG_INF("  *** Open this URL to configure auto-tune and other settings");
            } else if (strcmp(cmnd, "sat_pos_oled") == 0) {
                /* Satellite position for world map: [x, y] in 128x64 pixel coords */
                const char *p = strchr((char *)rx_payload, '[');
                if (p) {
                    p++;
                    tinygs_radio.sat_pos_x = strtof(p, (char **)&p);
                    while (*p == ',' || *p == ' ') p++;
                    tinygs_radio.sat_pos_y = strtof(p, NULL);
                }
                LOG_DBG("  → sat_pos: %.0f,%.0f",
                        (double)tinygs_radio.sat_pos_x,
                        (double)tinygs_radio.sat_pos_y);
            } else if (strncmp(cmnd, "frame/", 6) == 0) {
                int frame_num = atoi(cmnd + 6);
                tinygs_display_set_remote_frame(frame_num,
                                                 (const char *)rx_payload, ret);
            } else if (strcmp(cmnd, "frame") == 0) {
                LOG_DBG("  → frame command without number, ignored");
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

    /* First connect: clean_session=1 (fresh subscriptions).
     * Reconnect after network drop: clean_session=0 (broker keeps
     * subscriptions and delivers queued messages). */
    mqtt_client.clean_session = mqtt_first_connect ? 1 : 0;

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
    "</script>";
/* Closing tags added after commissioning info block */

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
        /* Write index.html with commissioning info (only if missing) */
        if (fs_stat("/NAND:/index.html", &entry) != 0) {
            /* Get EUI-64 for commissioning info */
            extern char device_client_id[13];
            if (device_client_id[0] == '\0') {
                uint64_t dev_id = NRF_FICR->DEVICEID[0] |
                                  ((uint64_t)NRF_FICR->DEVICEID[1] << 32);
                snprintf(device_client_id, sizeof(device_client_id),
                         "%04X%08X", (unsigned)(dev_id >> 32), (unsigned)dev_id);
            }

            fs_file_t_init(&file);
            if (fs_open(&file, "/NAND:/index.html", FS_O_CREATE | FS_O_WRITE) == 0) {
                /* Write static HTML head */
                fs_write(&file, html_content, strlen(html_content));

                /* Append commissioning info */
                static char commission_html[256];
                int clen = snprintf(commission_html, sizeof(commission_html),
                    "<div class='card' style='margin-top:16px;'>"
                    "<h2>Thread Commissioning</h2>"
                    "<p><strong>MAC / EUI-64:</strong> %s</p>"
                    "<p><strong>Joiner PSKd:</strong> %s</p>"
                    "<p style='font-size:0.8em;color:#666;'>"
                    "Run on your OTBR: <code>ot-ctl commissioner joiner add '*' %s</code></p>"
                    "</div></body></html>",
                    device_client_id,
                    CONFIG_OPENTHREAD_JOINER_PSKD,
                    CONFIG_OPENTHREAD_JOINER_PSKD);
                fs_write(&file, commission_html, clen);
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
            static char cfg_buf[256]; /* config.json is ~200 bytes */
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

                    p = strstr(cfg_buf, "\"display_timeout\":");
                    if (p) {
                        int t = atoi(p + 18);
                        if (t >= 0) tinygs_display_set_timeout((uint32_t)t);
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
                    "  \"alt\": %.0f,\n"
                    "  \"display_timeout\": 30\n"
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
/* -------------------------------------------------------------------------- */
/* RadioLib — radio type selected by DTS compatible                             */
/* -------------------------------------------------------------------------- */

static ZephyrHal radio_hal(lora_spi.bus, (struct spi_config *)&lora_spi.config);
static Module *radio_mod = nullptr;
/* Radio type from DTS compatible — compile-time selection */
#define LORA_NODE DT_ALIAS(lora0)
#if DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1262)
  SX1262 *radio = nullptr;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1268)
  SX1268 *radio = nullptr;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1276)
  SX1276 *radio = nullptr;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1278)
  SX1278 *radio = nullptr;
#else
  #error "Unsupported LoRa radio — check lora0 alias in app.overlay"
#endif

/* LoRa packet reception flag — set by DIO1 ISR */
static volatile bool lora_packet_received = false;

/* -------------------------------------------------------------------------- */
/* SNTP Time Sync                                                              */
/* -------------------------------------------------------------------------- */

static int64_t sntp_epoch_offset = 0; /* seconds: real_epoch = uptime_s + offset */
static bool time_synced = false;

static int64_t get_utc_epoch(void)
{
    if (!time_synced) return 0;
    return (int64_t)(k_uptime_get_32() / 1000) + sntp_epoch_offset;
}

/* SNTP callback — called from OpenThread context */
static void sntp_response_handler(void *aContext, uint64_t aTime, otError aResult)
{
    if (aResult == OT_ERROR_NONE) {
        uint32_t uptime_s = k_uptime_get_32() / 1000;
        sntp_epoch_offset = (int64_t)aTime - (int64_t)uptime_s;
        time_synced = true;
        LOG_INF("SNTP: synced, epoch=%llu", (unsigned long long)aTime);
    } else {
        LOG_WRN("SNTP: failed (%d)", (int)aResult);
    }
}

/**
 * Sync time via OpenThread SNTP client.
 * Uses Google NTP (2001:4860:4806:8::) — native IPv6, no NAT64 needed.
 */
static void sntp_sync(void)
{
    struct openthread_context *ot_ctx = openthread_get_default_context();
    if (!ot_ctx) return;

    otMessageInfo msgInfo;
    memset(&msgInfo, 0, sizeof(msgInfo));

    /* Google NTP IPv6: 2001:4860:4806:8:: */
    otIp6AddressFromString(OT_SNTP_DEFAULT_SERVER_IP, &msgInfo.mPeerAddr);
    msgInfo.mPeerPort = OT_SNTP_DEFAULT_SERVER_PORT;

    otSntpQuery query;
    query.mMessageInfo = &msgInfo;

    openthread_api_mutex_lock(ot_ctx);
    otError err = otSntpClientQuery(ot_ctx->instance, &query,
                                     sntp_response_handler, NULL);
    openthread_api_mutex_unlock(ot_ctx);

    if (err != OT_ERROR_NONE) {
        LOG_WRN("SNTP: query failed (%d)", (int)err);
    }
}

/* -------------------------------------------------------------------------- */
/* Doppler Compensation (P13 satellite propagator)                             */
/* -------------------------------------------------------------------------- */

#include "AioP13.h"

static uint32_t last_doppler_ms = 0;

/**
 * Compute Doppler shift using P13 propagator and apply to radio frequency.
 * Called every 4 seconds from the main loop. Requires valid TLE and time.
 *
 * NOTE: This requires real UTC time which we don't have (no NTP over Thread).
 * Doppler activates automatically when begine includes TLE data and
 * incorrect results without accurate time. When TLE data arrives and we
 * implement time sync, this will produce real Doppler corrections.
 */
static void doppler_update(void)
{
    if (!tinygs_radio.tle_valid || !tinygs_radio.doppler_enabled || !radio) {
        return;
    }

    /* SNTP time sync via OpenThread SNTP client → Google NTP IPv6 */
    if (!time_synced) {
        return; /* No SNTP time yet — can't predict orbits */
    }

    int64_t epoch = get_utc_epoch();
    /* Convert epoch to Y/M/D H:M:S */
    /* Simple conversion — good enough for satellite tracking */
    time_t t = (time_t)epoch;
    struct tm *utc = gmtime(&t);
    if (!utc) return;

    P13DateTime dt(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                   utc->tm_hour, utc->tm_min, utc->tm_sec);
    P13Observer obs("GS", (double)tinygs_station_lat,
                    (double)tinygs_station_lon,
                    (double)tinygs_station_alt);
    P13Satellite_tGS sat(tinygs_radio.tle);

    sat.predict(dt);

    /* Get elevation to check if satellite is above horizon */
    double elevation = 0.0, azimuth = 0.0;
    sat.elaz(obs, elevation, azimuth);

    if (elevation <= 0.0) {
        return; /* Below horizon — no Doppler needed */
    }

    /* Compute Doppler — doppler() takes MHz, returns shifted MHz */
    double rx_freq_mhz = (double)tinygs_radio.frequency;
    double doppler_freq_mhz = sat.doppler(rx_freq_mhz, P13_FRX);
    float new_doppler = (float)((doppler_freq_mhz - rx_freq_mhz) * 1e6);

    /* Hysteresis — only retune if change exceeds tolerance */
    if (fabsf(new_doppler - tinygs_radio.freq_doppler) > tinygs_radio.doppler_tol) {
        tinygs_radio.freq_doppler = new_doppler;
        float effective_freq = tinygs_radio.frequency +
                              (tinygs_radio.freq_offset + tinygs_radio.freq_doppler) / 1e6f;
        radio->setFrequency(effective_freq);
        radio->startReceive();
        LOG_INF("Doppler: %.0f Hz → %.6f MHz (el=%.1f°)",
                (double)new_doppler, (double)effective_freq, elevation);
    } else {
        LOG_DBG("Doppler: %.0f Hz (delta=%.0f < tol=%.0f, el=%.1f°)",
                (double)new_doppler,
                (double)fabsf(new_doppler - tinygs_radio.freq_doppler),
                (double)tinygs_radio.doppler_tol, elevation);
    }
}

/* -------------------------------------------------------------------------- */
/* LoRa RX                                                                     */
/* -------------------------------------------------------------------------- */

static void lora_rx_callback(void)
{
    lora_packet_received = true;
}

static void init_radio(void)
{
    LOG_INF("Initializing LoRa radio (%s)...", DT_NODE_FULL_NAME(LORA_NODE));

    uint32_t cs   = radio_hal.addPin(&lora_cs);
    uint32_t rst  = radio_hal.addPin(&lora_reset);
    uint32_t busy = radio_hal.addPin(&lora_busy);
    uint32_t dio1 = radio_hal.addPin(&lora_dio1);

    /* Run HAL smoke tests before radio init */
    /* HAL smoke tests disabled for production — saves ~1KB flash
     * extern int zephyr_hal_run_tests(ZephyrHal *hal);
     * zephyr_hal_run_tests(&radio_hal); */

    radio_mod = new Module(&radio_hal, cs, dio1, rst, busy);
#if DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1262)
    radio = new SX1262(radio_mod);
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1268)
    radio = new SX1268(radio_mod);
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1276)
    radio = new SX1276(radio_mod);
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1278)
    radio = new SX1278(radio_mod);
#endif

    int state = radio->begin();
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("Radio: %s initialized (RadioLib)",
                DT_NODE_FULL_NAME(DT_NODELABEL(sx1262)));
    } else {
        LOG_ERR("Radio init failed: %d", state);
        return;
    }

    /* Set default LoRa config — 433MHz, SF10, BW125, CR5
     * This will be overridden by server batch_conf commands */
    state = radio->setFrequency(TINYGS_DEFAULT_FREQ);
    if (state != RADIOLIB_ERR_NONE) {
        LOG_ERR("setFrequency failed: %d", state);
    }
    radio->setSpreadingFactor(10);
    radio->setBandwidth(250.0);
    radio->setCodingRate(5);
    radio->setSyncWord(0x12);
    radio->autoLDRO();
    radio->setRxBoostedGainMode(true);

    /* Register DIO1 interrupt callback and start receiving */
    radio->setPacketReceivedAction(lora_rx_callback);
    state = radio->startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("LoRa listening on %.3f MHz, SF%d, BW%.0f",
                (double)tinygs_radio.frequency, tinygs_radio.sf,
                (double)tinygs_radio.bw);
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
        lora_rx_count++;
        /* Flash LED1 white on packet RX */
        neopixel_set(NEO_OFF,  NEO_WHITE); /* LED1 flash on LoRa RX */
        LOG_INF("LoRa RX: %u bytes, RSSI=%.1f, SNR=%.1f, FreqErr=%.1f",
                (unsigned)len, (double)rssi, (double)snr, (double)freq_err);

        /* Apply packet filter if active.
         * filter[0] = count of bytes to match
         * filter[1] = byte position in packet to start matching
         * filter[2..N] = expected byte values */
        if (tinygs_radio.filter[0] > 0) {
            uint8_t count = tinygs_radio.filter[0];
            uint8_t start = tinygs_radio.filter[1];
            bool filtered = false;
            for (uint8_t i = 0; i < count && i + 2 < sizeof(tinygs_radio.filter); i++) {
                if (start + i >= len || data[start + i] != tinygs_radio.filter[2 + i]) {
                    filtered = true;
                    break;
                }
            }
            if (filtered) {
                LOG_DBG("LoRa RX: filtered out (no match at offset %d)", start);
                radio->startReceive();
                return true;
            }
        }

        /* Notify display of packet reception */
        tinygs_display_packet_rx(rssi, snr);

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

    /* Restart reception, turn off RX flash */
    radio->startReceive();
    neopixel_off(); /* Back to off after RX flash */
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

    LOG_INF("=== TinyGS nRF52 v%u — Thread/MQTT-TLS ===", (unsigned)TINYGS_VERSION);

    /* Log boot/reset reason — read raw nRF RESETREAS for full picture.
     * Errata 136: after pin reset, other bits may be falsely set. */
    {
        uint32_t resetreas = NRF_POWER->RESETREAS;
        NRF_POWER->RESETREAS = resetreas; /* Clear by writing 1s */
        uint32_t gpregret = nrf_power_gpregret_get(NRF_POWER, 0);

        LOG_INF("Boot: RESETREAS=0x%08x gpregret=0x%02x [%s%s%s%s%s%s]",
                resetreas, gpregret,
                (resetreas & (1<<0)) ? "PIN " : "",
                (resetreas & (1<<1)) ? "DOG " : "",
                (resetreas & (1<<2)) ? "SREQ " : "",
                (resetreas & (1<<3)) ? "LOCKUP " : "",
                (resetreas & (1<<16)) ? "OFF " : "",
                (resetreas & (1<<17)) ? "DIF " : "");

        if (gpregret == 0x57) {
            LOG_INF("  → 1200-baud bootloader entry");
        }
        if (resetreas & (1<<3)) {
            LOG_ERR("  → CPU LOCKUP detected! (fault-in-fault-handler)");
        }
    }

    /* Check for crash diagnostic from previous boot (retained RAM) */
    if ((crash_reason & 0xFFFF0000) == CRASH_MAGIC) {
        LOG_ERR("*** PREVIOUS CRASH: reason=%u PC=0x%08x LR=0x%08x ***",
                (unsigned)(crash_reason & 0xFFFF),
                (unsigned)crash_pc, (unsigned)crash_lr);
        LOG_ERR("Use: arm-zephyr-eabi-addr2line -e build/zephyr/zephyr.elf 0x%08x",
                (unsigned)crash_pc);
        crash_reason = 0; /* Clear so we don't report again */
    }

    log_heap_usage("boot");
    init_openthread();

    /* Check if device has Thread credentials (commissioned).
     * If not, stay fully awake for 15 minutes to allow Joiner commissioning.
     * The user needs time to scan the QR code from index.html. */
    {
        struct openthread_context *ot_ctx = openthread_get_default_context();
        if (ot_ctx) {
            openthread_api_mutex_lock(ot_ctx);
            bool commissioned = otDatasetIsCommissioned(ot_ctx->instance);
            openthread_api_mutex_unlock(ot_ctx);

            if (!commissioned) {
                LOG_INF("*** COMMISSIONING MODE ***");
                LOG_INF("Device not provisioned — staying awake for 15 minutes");
                LOG_INF("Joiner PSKd: %s", CONFIG_OPENTHREAD_JOINER_PSKD);
                LOG_INF("Scan QR code from index.html or run:");
                LOG_INF("  ot-ctl commissioner joiner add '*' %s",
                        CONFIG_OPENTHREAD_JOINER_PSKD);
                /* Keep display on during commissioning */
                tinygs_display_set_timeout(900); /* 15 minutes */
            } else {
                LOG_INF("Thread credentials found — normal mode");
            }
        }
    }

    /* Load persistent config from NVS. Must be AFTER init_openthread()
     * because OpenThread initializes the settings/NVS subsystem.
     * No extra CONFIG_SETTINGS needed in prj.conf — OpenThread provides it.
     * config.json values are loaded first (in setup_usb_storage), then
     * NVS overrides with any previously saved values. */
    tinygs_config_init();
    tinygs_display_init();
    watchdog_init();
    led_init();
    breathing_led_init();
    init_radio();

    int retry_count = 0;
    int mqtt_poll_fd_count = 0;
    struct zsock_pollfd mqtt_poll_fd;

    while (1) {
        switch (app_state) {

        case STATE_WAIT_THREAD:
            /* Blue blink while waiting for Thread */
            neopixel_set(0, 0, (k_uptime_get_32() / 500) % 2 ? 20 : 0,  NEO_OFF); /* Blue blink = waiting for Thread */
            if (thread_attached) {
                /* Heap stats now in periodic STATUS log */
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
            /* Heap stats now in periodic STATUS log */
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
            /* Heap stats now in periodic STATUS log */
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
            LOG_INF("MQTT connected, entering main loop");
            neopixel_off(); /* NeoPixels off when stable */
            breathing_led_start(); /* Green LED breathes when connected */

            /* Sync time via SNTP — needed for Doppler compensation */
            if (!time_synced) {
                sntp_sync();
            }

            /* TinyGS main loop — process MQTT, LoRa RX, and pings */
            uint32_t last_ping_ms = k_uptime_get_32();
            uint32_t last_status_log_ms = last_ping_ms;
            #define STATUS_LOG_INTERVAL_MS 300000 /* 5 minutes */

            while (app_state == STATE_MQTT_CONNECTED) {
                /* Poll MQTT socket for incoming data */
                int rc = zsock_poll(&mqtt_poll_fd, mqtt_poll_fd_count, 100);
                if (rc > 0) {
                    mqtt_input(&mqtt_client);
                }
                {
                    int live_ret = mqtt_live(&mqtt_client);
                    if (live_ret && live_ret != -EAGAIN) {
                        LOG_DBG("mqtt_live: %d", live_ret);
                    }
                }

                /* Check for LoRa packet reception */
                lora_check_rx();

                /* Update display (~every 100ms from poll timeout) */
                tinygs_display_update();

                /* Doppler compensation — every 4s if TLE available */
                uint32_t now_ms = k_uptime_get_32();
                if ((now_ms - last_doppler_ms) >= doppler_interval_ms) {
                    doppler_update();
                    last_doppler_ms = now_ms;
                }

                /* Send TinyGS ping (also serves as MQTT keepalive — see protocol doc) */
                if ((now_ms - last_ping_ms) >= (TINYGS_PING_INTERVAL_S * 1000)) {
                    tinygs_send_ping(&mqtt_client, cfg_mqtt_user, cfg_station);
                    last_ping_ms = now_ms;
                }

                /* Weblogin request via BOOT button */
                if (tinygs_display_weblogin_requested()) {
                    tinygs_send_weblogin_request(&mqtt_client, cfg_mqtt_user, cfg_station);
                }

                /* Track main thread stack high-water mark */
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
                {
                    static size_t stack_hwm = SIZE_MAX;
                    size_t unused = 0;
                    if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
                        if (unused < stack_hwm) stack_hwm = unused;
                    }
                }
#endif

                /* Periodic status log — every 5 minutes */
                if ((now_ms - last_status_log_ms) >= STATUS_LOG_INTERVAL_MS) {
                    uint32_t uptime_s = k_uptime_get_32() / 1000;
                    uint32_t conn_s = (now_ms - mqtt_connected_uptime_ms) / 1000;

                    /* Get main thread stack usage */
                    size_t stack_unused = 0;
                    size_t stack_size = CONFIG_MAIN_STACK_SIZE;
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
                    k_thread_stack_space_get(k_current_get(), &stack_unused);
#endif
                    size_t stack_used = stack_size - stack_unused;

#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
                    struct sys_memory_stats stats;
                    sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
                    LOG_INF("STATUS: up=%us conn=%us mqtt_rx=%u lora_rx=%u "
                            "heap=%u/%u(peak=%u) stack=%u/%u "
                            "vbat=%dmV sat=%s",
                            (unsigned)uptime_s, (unsigned)conn_s,
                            (unsigned)mqtt_rx_count, (unsigned)lora_rx_count,
                            (unsigned)stats.allocated_bytes,
                            (unsigned)CONFIG_HEAP_MEM_POOL_SIZE,
                            (unsigned)stats.max_allocated_bytes,
                            (unsigned)stack_used, (unsigned)stack_size,
                            read_vbat_mv(),
                            tinygs_radio.satellite);
#else
                    LOG_INF("STATUS: up=%us conn=%us mqtt_rx=%u lora_rx=%u "
                            "stack=%u/%u vbat=%dmV sat=%s",
                            (unsigned)uptime_s, (unsigned)conn_s,
                            (unsigned)mqtt_rx_count, (unsigned)lora_rx_count,
                            (unsigned)stack_used, (unsigned)stack_size,
                            read_vbat_mv(),
                            tinygs_radio.satellite);
#endif
                    last_status_log_ms = now_ms;
                }

                k_msleep(100);  /* 100ms loop — responsive to LoRa packets */
            }
            break;
        }

        case STATE_ERROR:
            neopixel_set(NEO_RED,  NEO_OFF); /* Red = error */
            breathing_led_stop();
            led_set(false);
            LOG_ERR("Error state. Retrying in 30s...");
            k_msleep(30000);
            app_state = thread_attached ? STATE_DNS_RESOLVE : STATE_WAIT_THREAD;
            retry_count = 0;
            break;
        }
    }

    return 0;
}
