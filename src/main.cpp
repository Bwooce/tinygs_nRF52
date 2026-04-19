#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <openthread/thread.h>
#include <openthread/error.h>
#include <openthread/netdata.h>
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
#include "bitcode.h"
#if defined(CONFIG_IOT_LOG)
#include <iot_log_zephyr.h>
#endif

LOG_MODULE_REGISTER(tinygs_nrf52, LOG_LEVEL_DBG);

/* Translate common negative errno values to symbolic names. Values
 * match Zephyr minimal libc errno.h, which differs from POSIX above
 * 100. Falls back to "?" so %s is safe. Declared extern "C" in
 * tinygs_protocol.h so other TUs can call it. */
extern "C" const char *errno_name(int err)
{
    switch (err < 0 ? -err : err) {
    case 0:   return "OK";
    case 5:   return "EIO";
    case 11:  return "EAGAIN";
    case 12:  return "ENOMEM";
    case 22:  return "EINVAL";
    case 61:  return "ENODATA";
    case 104: return "ECONNRESET";
    case 105: return "ENOBUFS";
    case 110: return "ESHUTDOWN";
    case 111: return "ECONNREFUSED";
    case 113: return "ECONNABORTED";
    case 114: return "ENETUNREACH";
    case 115: return "ENETDOWN";
    case 116: return "ETIMEDOUT";
    case 117: return "EHOSTDOWN";
    case 118: return "EHOSTUNREACH";
    case 119: return "EINPROGRESS";
    case 128: return "ENOTCONN";
    case 134: return "ENOTSUP";
    case 140: return "ECANCELED";
    default:  return "?";
    }
}

/* RadioLib status/error codes (from lib/RadioLib/src/TypeDef.h). Covers
 * only the codes we realistically see on SX1262 LoRa/FSK begin and RX
 * paths — adding everything would be ~70 entries and most aren't
 * reachable on this chip. */
static const char *radio_err_name(int err)
{
    switch (err) {
    case 0:    return "OK";
    case -1:   return "UNKNOWN";
    case -2:   return "CHIP_NOT_FOUND";
    case -3:   return "MEMORY_ALLOCATION_FAILED";
    case -4:   return "PACKET_TOO_LONG";
    case -5:   return "TX_TIMEOUT";
    case -6:   return "RX_TIMEOUT";
    case -7:   return "CRC_MISMATCH";
    case -8:   return "INVALID_BANDWIDTH";
    case -9:   return "INVALID_SPREADING_FACTOR";
    case -10:  return "INVALID_CODING_RATE";
    case -12:  return "INVALID_FREQUENCY";
    case -13:  return "INVALID_OUTPUT_POWER";
    case -16:  return "SPI_WRITE_FAILED";
    case -17:  return "INVALID_CURRENT_LIMIT";
    case -18:  return "INVALID_PREAMBLE_LENGTH";
    case -19:  return "INVALID_GAIN";
    case -20:  return "WRONG_MODEM";
    case -24:  return "LORA_HEADER_DAMAGED";
    case -25:  return "UNSUPPORTED";
    case -28:  return "NULL_POINTER";
    case -30:  return "PACKET_TOO_SHORT";
    case -101: return "INVALID_BIT_RATE";
    case -102: return "INVALID_FREQUENCY_DEVIATION";
    case -103: return "INVALID_BIT_RATE_BW_RATIO";
    case -104: return "INVALID_RX_BANDWIDTH";
    case -105: return "INVALID_SYNC_WORD";
    case -106: return "INVALID_DATA_SHAPING";
    case -107: return "INVALID_MODULATION";
    case -701: return "INVALID_CRC_CONFIGURATION";
    case -703: return "INVALID_TCXO_VOLTAGE";
    case -704: return "INVALID_MODULATION_PARAMETERS";
    case -705: return "SPI_CMD_TIMEOUT";
    case -706: return "SPI_CMD_INVALID";
    case -707: return "SPI_CMD_FAILED";
    default:   return "?";
    }
}

/* Forward declarations for radio RX */
static void lora_rx_callback(void);
static volatile bool lora_packet_received = false;

/* Crash diagnostic — __noinit survives warm reset (SREQ doesn't clear RAM) */
#define CRASH_MAGIC 0xDEAD0000
static volatile uint32_t __noinit crash_reason;
static volatile uint32_t __noinit crash_pc;
static volatile uint32_t __noinit crash_lr;
static volatile uint32_t __noinit crash_icsr;  /* SCB->ICSR — VECTACTIVE identifies the IRQ */
static char __noinit crash_thread[16];

extern "C" void k_sys_fatal_error_handler(unsigned int reason,
                                           const z_arch_esf_t *esf)
{
    crash_reason = CRASH_MAGIC | (reason & 0xFFFF);
    crash_icsr = *(volatile uint32_t *)0xE000ED04; /* SCB->ICSR */
    if (esf) {
        crash_pc = esf->basic.pc;
        crash_lr = esf->basic.lr;
    }
    const char *tname = k_thread_name_get(k_current_get());
    if (tname) {
        strncpy(crash_thread, tname, sizeof(crash_thread) - 1);
        crash_thread[sizeof(crash_thread) - 1] = '\0';
    } else {
        crash_thread[0] = '\0';
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
    /* Don't configure P1.03 as GPIO — PWM0 will own this pin for breathing LED.
     * The GPIO config would conflict with the PWM peripheral. */
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
#include <nrfx_glue.h>   /* nrfx_isr — required for runtime IRQ binding */
#include <hal/nrf_gpio.h>

static nrfx_pwm_t pwm_led = NRFX_PWM_INSTANCE(0);
static bool pwm_breathing = false;

/* Breathing LED: one gentle pulse, then 25s off, repeat.
 * Uses PWM event handler + k_timer for the pause (end_delay doesn't
 * work with NRFX_PWM_FLAG_LOOP). */
#define BREATH_MAX_DUTY  50    /* 5% max brightness */
#define BREATH_STEPS     64
#define BREATH_PAUSE_S   25    /* Seconds between pulses */
static nrf_pwm_values_individual_t breath_values[BREATH_STEPS];
static nrf_pwm_sequence_t breath_seq = {
    .values = { .p_individual = breath_values },
    .length = NRF_PWM_VALUES_LENGTH(breath_values),
    .repeats = 40,    /* Hold each step ~40ms. Pulse duration: 64*40ms ≈ 2.6s */
    .end_delay = 0,
};

static void breath_timer_handler(struct k_timer *timer);
static K_TIMER_DEFINE(breath_timer, breath_timer_handler, NULL);

static void breath_pwm_handler(nrfx_pwm_evt_type_t event_type, void *p_context)
{
    if (event_type == NRFX_PWM_EVT_FINISHED) {
        /* Pulse complete — start timer for the pause */
        k_timer_start(&breath_timer, K_SECONDS(BREATH_PAUSE_S), K_NO_WAIT);
    }
}

static void breath_timer_handler(struct k_timer *timer)
{
    /* Pause complete — play one more pulse */
    if (pwm_breathing) {
        nrfx_pwm_simple_playback(&pwm_led, &breath_seq, 1, 0);
    }
}

static void breathing_led_init(void)
{
    /* Build breathing table. pin_inverted handles active-low LED,
     * so 0=off, BREATH_MAX_DUTY=max brightness. */
    for (int i = 0; i < BREATH_STEPS; i++) {
        int brightness = (i < 32) ? i : (63 - i);
        int duty = brightness * brightness * BREATH_MAX_DUTY / (31 * 31);
        breath_values[i].channel_0 = (uint16_t)duty;
        breath_values[i].channel_1 = 0;
        breath_values[i].channel_2 = 0;
        breath_values[i].channel_3 = 0;
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

    /* Hook PWM0 IRQ vector to the nrfx dispatcher. Zephyr's nrfx
     * integration doesn't auto-connect when we use nrfx_pwm directly,
     * so the IRQ fires into a NULL vector → K_ERR_SPURIOUS_IRQ.
     * IRQ_CONNECT doesn't build in C++, so register dynamically.
     * Requires CONFIG_DYNAMIC_INTERRUPTS=y.
     *
     * Use `nrfx_isr` as the ISR trampoline with the nrfx handler passed
     * as context — this is Zephyr's canonical pattern (see
     * ncs/zephyr/modules/hal_nordic/nrfx/nrfx_glue.c). The previous
     * direct-cast form only worked by accident of ARM calling
     * convention (unused R0 arg) and would be fragile across nrfx
     * versions. Priority matches nrfx's own default so the LED IRQ
     * doesn't preempt higher-priority nrfx peripherals. */
    irq_connect_dynamic(PWM0_IRQn,
                        NRFX_PWM_DEFAULT_CONFIG_IRQ_PRIORITY,
                        nrfx_isr,
                        (const void *)nrfx_pwm_0_irq_handler,
                        0);
    irq_enable(PWM0_IRQn);
    if (nrfx_pwm_init(&pwm_led, &config, breath_pwm_handler, NULL) == NRFX_SUCCESS) {
        /* Play a single-step "off" sequence to ensure LED starts dark.
         * Without this, the PWM idle state may leave the pin floating. */
        static nrf_pwm_values_individual_t off_val = { 0, 0, 0, 0 };
        static nrf_pwm_sequence_t off_seq = {
            .values = { .p_individual = &off_val },
            .length = NRF_PWM_VALUES_LENGTH(off_val),
            .repeats = 0,
            .end_delay = 0,
        };
        nrfx_pwm_simple_playback(&pwm_led, &off_seq, 1, 0);
        LOG_INF("Breathing LED: PWM0 on P1.03");
    }
}

static void breathing_led_start(void)
{
    if (!pwm_breathing) {
        pwm_breathing = true;
        nrfx_pwm_simple_playback(&pwm_led, &breath_seq, 1, 0); /* play once, handler restarts */
    }
}

static void breathing_led_stop(void)
{
    if (pwm_breathing) {
        pwm_breathing = false;
        k_timer_stop(&breath_timer);
        nrfx_pwm_stop(&pwm_led, false);
        /* PWM released — manually drive pin to turn LED off.
         * Active-low LED: HIGH = off */
        nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1, 3));
        nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1, 3));
    }
}

static void led_set(bool on)
{
    /* P1.03 is now PWM-driven (breathing LED). Use breathing_led_start/stop instead.
     * This function kept for error state where we want solid on/off. */
    (void)on;
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
        static otDeviceRole prev_role = OT_DEVICE_ROLE_DISABLED;
        static int64_t detached_at_ms = 0;
        otDeviceRole role = otThreadGetDeviceRole(ot_context->instance);

        bool was_attached = (prev_role == OT_DEVICE_ROLE_CHILD ||
                             prev_role == OT_DEVICE_ROLE_ROUTER ||
                             prev_role == OT_DEVICE_ROLE_LEADER);
        bool now_attached = (role == OT_DEVICE_ROLE_CHILD ||
                             role == OT_DEVICE_ROLE_ROUTER ||
                             role == OT_DEVICE_ROLE_LEADER);

        if (!was_attached && now_attached && detached_at_ms != 0) {
            int32_t detached_s = (int32_t)((k_uptime_get() - detached_at_ms) / 1000);
            LOG_WRN("Thread re-attached as %s after %ds detached",
                    ot_role_str(role), detached_s);
            detached_at_ms = 0;
        } else if (was_attached && !now_attached) {
            detached_at_ms = k_uptime_get();
            LOG_WRN("Thread parent lost (role: %s)", ot_role_str(role));
        } else {
            LOG_INF("Thread role: %s", ot_role_str(role));
        }

        thread_attached = now_attached;
        prev_role = role;
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
        LOG_ERR("Failed to get active dataset: %d (%s)",
                (int)err, otThreadErrorToString(err));
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
        LOG_ERR("Joiner failed: %d (%s)", (int)error, otThreadErrorToString(error));
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
            LOG_ERR("otJoinerStart failed: %d (%s)", (int)err, otThreadErrorToString(err));
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
        LOG_ERR("OT DNS callback error: %d (%s)", (int)aError, otThreadErrorToString(aError));
        dns_result = -1;
        k_sem_give(&dns_sem);
        return;
    }

    otIp6Address addr;
    uint32_t ttl;
    otError err = otDnsAddressResponseGetAddress(aResponse, 0, &addr, &ttl);
    if (err != OT_ERROR_NONE) {
        LOG_ERR("OT DNS no address in response: %d (%s)", (int)err, otThreadErrorToString(err));
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

    k_sem_reset(&dns_sem);
    dns_result = -1;

    /* Iterate the Thread network data's external routes to find the
     * NAT64 prefix advertised by whichever peer BR is primary translator.
     * otBorderRoutingGetFavoredNat64Prefix() is BR-only and not linked on
     * an MTD build, but the prefix IS reachable via netdata which is
     * fully available on MTDs. We synthesise a DNS server address =
     * NAT64_prefix + 1.1.1.1 so every DNS query leaves the mesh via
     * NAT64 and reaches Cloudflare as a normal IPv4 UDP query. Stays
     * mesh-scoped on our side, no ULA→global egress issue. */
    otIp6Prefix nat64_prefix;
    memset(&nat64_prefix, 0, sizeof(nat64_prefix));
    bool found = false;
    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otExternalRouteConfig route;
    openthread_api_mutex_lock(ctx);
    while (otNetDataGetNextRoute(ctx->instance, &it, &route) == OT_ERROR_NONE) {
        if (route.mNat64 && route.mPrefix.mLength == 96) {
            nat64_prefix = route.mPrefix;
            found = true;
            break;
        }
    }
    openthread_api_mutex_unlock(ctx);
    if (!found) {
        LOG_ERR("No /96 NAT64 route in Thread netdata — mesh has no NAT64 translator?");
        return -1;
    }

    otDnsQueryConfig config;
    memset(&config, 0, sizeof(config));
    config.mServerSockAddr.mAddress = nat64_prefix.mPrefix;
    /* Append IPv4 1.1.1.1 into the last 4 bytes (RFC 6052 /96 synthesis) */
    config.mServerSockAddr.mAddress.mFields.m8[12] = 1;
    config.mServerSockAddr.mAddress.mFields.m8[13] = 1;
    config.mServerSockAddr.mAddress.mFields.m8[14] = 1;
    config.mServerSockAddr.mAddress.mFields.m8[15] = 1;
    config.mServerSockAddr.mPort    = 53;
    config.mResponseTimeout         = 10000;
    config.mMaxTxAttempts           = 3;
    config.mRecursionFlag           = OT_DNS_FLAG_RECURSION_DESIRED;
    config.mNat64Mode               = OT_DNS_NAT64_ALLOW;

    char dns_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&config.mServerSockAddr.mAddress, dns_str, sizeof(dns_str));
    LOG_INF("Resolving %s via NAT64-synthesised DNS [%s]:53",
            MQTT_BROKER_HOSTNAME, dns_str);

    /* Query A record (IPv4) explicitly. mqtt.tinygs.com is IPv4-only,
     * so AAAA queries return NotFound. otDnsClientResolveIp4Address
     * queries A and — with mNat64Mode=ALLOW and a NAT64 prefix in
     * netdata — synthesises an AAAA for the caller via OT's built-in
     * NAT64 translation. We get back a NAT64-prefixed address we can
     * connect to directly. */
    openthread_api_mutex_lock(ctx);
    otError err = otDnsClientResolveIp4Address(ctx->instance,
                                               MQTT_BROKER_HOSTNAME,
                                               dns_callback, NULL,
                                               &config);
    openthread_api_mutex_unlock(ctx);

    if (err != OT_ERROR_NONE) {
        LOG_ERR("otDnsClientResolveAddress failed: %d (%s)",
                (int)err, otThreadErrorToString(err));
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
        LOG_WRN("MQTT DISCONNECTED after %us: result=%d (%s)",
                (unsigned)(now - mqtt_connected_uptime_ms) / 1000,
                evt->result, errno_name(evt->result));
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
        /* Sized to match TINYGS_BEGINE_MAX_LEN — parser's stated ceiling.
         * Silent truncation here would hand invalid JSON to the parser. */
        static uint8_t rx_payload[TINYGS_BEGINE_MAX_LEN];
        uint32_t topic_len = MIN(pub->message.topic.topic.size, sizeof(rx_topic) - 1);
        memcpy(rx_topic, pub->message.topic.topic.utf8, topic_len);
        rx_topic[topic_len] = '\0';

        if (pub->message.payload.len >= sizeof(rx_payload)) {
            LOG_WRN("MQTT RX payload %u exceeds buffer %zu — will truncate",
                    (unsigned)pub->message.payload.len, sizeof(rx_payload));
        }
        uint32_t payload_len = MIN(pub->message.payload.len, sizeof(rx_payload) - 1);
        int ret = mqtt_read_publish_payload(client, rx_payload, payload_len);
        if (ret < 0) {
            LOG_ERR("MQTT payload read error: %d (%s)", ret, errno_name(ret));
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

            if (strcmp(cmnd, "beginp") == 0) {
                /* beginp = "persist as boot default, do NOT reconfigure live"
                 * (matches ESP32 MQTT_Client.cpp:669 setModemStartup).
                 * Server sends this to update the station's next-boot modem
                 * config; a follow-up `begine` handles the live tune. */
                size_t n = strlen((char *)rx_payload);
                if (n >= sizeof(tinygs_radio.modem_conf)) {
                    LOG_WRN("  → beginp: payload %zu > modem_conf buf, skipped", n);
                } else {
                    /* Validate we can at least deserialize it before saving —
                     * no point persisting a blob we couldn't load. */
                    struct tinygs_begine_msg tmp;
                    int64_t rc = tinygs_parse_begine((char *)rx_payload, ret, &tmp);
                    if (rc < 0 || !tmp.mode) {
                        LOG_WRN("  → beginp: invalid config, not persisting");
                    } else {
                        tinygs_config_save_modem_conf((char *)rx_payload);
                        LOG_INF("  → beginp: modem_conf persisted (%zu bytes)", n);
                    }
                }
            } else if (strcmp(cmnd, "begine") == 0 ||
                strcmp(cmnd, "batch_conf") == 0) {
                if (radio != nullptr) {
                    /* Disable RX interrupt before reconfiguring radio.
                     * Prevents race where a packet received on the old config
                     * gets processed with the new satellite's filter/metadata.
                     * ESP32 does the same: clearPacketReceivedAction() + disableInterrupt() */
                    radio->clearPacketReceivedAction();

                    /* Drain any pending packet before reconfig */
                    if (lora_packet_received) {
                        lora_packet_received = false;
                    }

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
                        /* Common fields */
                        float freq = tinygs_begine_get_freq(&msg);
                        if (freq > 100.0f && freq < 1000.0f) {
                            tinygs_radio.frequency = freq;
                        }

                        if (msg.sat) {
                            strncpy(tinygs_radio.satellite, msg.sat,
                                    sizeof(tinygs_radio.satellite) - 1);
                            tinygs_radio.satellite[sizeof(tinygs_radio.satellite) - 1] = '\0';
                        }
                        tinygs_radio.norad = (uint32_t)msg.NORAD;

                        bool is_fsk = msg.mode && strcmp(msg.mode, "LoRa") != 0;
                        strncpy(tinygs_radio.modem_mode,
                                is_fsk ? "FSK" : "LoRa",
                                sizeof(tinygs_radio.modem_mode) - 1);

                        if (!is_fsk) {
                            /* ---- LoRa mode ---- */
                            radio->setFrequency(freq + tinygs_radio.freq_offset / 1e6f);
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
                            tinygs_radio.pl = msg.pl;
                            tinygs_radio.crc_on = msg.crc;
                            tinygs_radio.fldro = msg.fldro;

                            if (msg.fldro == 2) {
                                radio->autoLDRO();
                            } else {
                                radio->forceLDRO(msg.fldro ? true : false);
                            }

                            radio->setCRC(msg.crc ? 2 : 0);
                            radio->invertIQ(msg.iIQ);
                            tinygs_radio.iIQ = msg.iIQ;

                            /* Implicit vs explicit header: ESP32 uses "len" field,
                             * NOT "cl". "cl" is content length for display.
                             * "len" defaults to 0 = explicit header. */
                            if (msg.len > 0) {
                                radio->implicitHeader(msg.len);
                            } else {
                                radio->explicitHeader();
                            }

                            radio->setRxBoostedGainMode(true);
                        } else {
                            /* ---- FSK mode ---- */
                            float bw = tinygs_begine_get_bw(&msg);
                            float fd = tinygs_begine_get_fd(&msg);
                            float br = tinygs_begine_get_br(&msg);
                            tinygs_radio.bw = bw;
                            tinygs_radio.bitrate = br;
                            tinygs_radio.freq_dev = fd;
                            tinygs_radio.ook = msg.ook;
                            tinygs_radio.fsk_len = msg.len;

                            /* Server's "br" and "fd" are already in kbps and
                             * kHz respectively (ESP32 station hands them to
                             * RadioLib unscaled). Don't divide. */
                            /* TODO(debug): verbose log — confirms -707 / unit
                             * regressions if they recur. Keep while we're
                             * still hitting new FSK sat configs for the
                             * first time. Remove once we've seen a week
                             * of clean FSK inits and a real RX. */
                            LOG_INF("  beginFSK args: freq=%.4f br=%.3f fd=%.3f bw=%.1f pwr=%d pl=%d tcxo=%.1f",
                                    (double)(freq + tinygs_radio.freq_offset / 1e6f),
                                    (double)br, (double)fd, (double)bw,
                                    msg.pwr, msg.pl, (double)LORA_TCXO_VOLTAGE);
                            int16_t rc = radio->beginFSK(
                                freq + tinygs_radio.freq_offset / 1e6f,
                                br,                      /* kbps */
                                fd,                      /* kHz */
                                bw,                      /* kHz */
                                msg.pwr,
                                msg.pl,
                                LORA_TCXO_VOLTAGE);
                            if (rc != RADIOLIB_ERR_NONE) {
                                LOG_ERR("beginFSK failed: %d (%s)", rc, radio_err_name(rc));
                            }

                            /* Re-register DIO1 interrupt (beginFSK resets it) */
                            radio->setPacketReceivedAction(lora_rx_callback);

                            radio->setCRC(0); /* FSK HW CRC off — done in software */

                            if (msg.len > 0) {
                                radio->fixedPacketLengthMode(msg.len);
                            }

                            /* FSK sync word from "fsw" array in JSON */
                            uint8_t fsw_buf[8];
                            int fsw_len = tinygs_parse_fsw(
                                tinygs_radio.modem_conf,
                                strlen(tinygs_radio.modem_conf),
                                fsw_buf, sizeof(fsw_buf));
                            if (fsw_len > 0) {
                                radio->setSyncWord(fsw_buf, fsw_len);
                            }

                            /* OOK / data shaping */
                            if (msg.ook == 255) {
                                radio->setDataShaping(RADIOLIB_SHAPING_1_0);
                            } else if (msg.ook > 0) {
                                radio->setDataShaping((float)msg.ook);
                            } else {
                                radio->setDataShaping(RADIOLIB_SHAPING_NONE);
                            }

                            /* Encoding: 0=NRZ, 1=Manchester, 2=whitening */
                            radio->setEncoding(msg.enc);
                            if (msg.enc == 2) {
                                radio->setWhitening(true, msg.ws);
                            }
                            tinygs_radio.fsk_enc = msg.enc;
                            tinygs_radio.fsk_framing = msg.fr;

                            /* Software CRC config — stored for post-RX check */
                            tinygs_radio.sw_crc_enabled = msg.cSw;
                            tinygs_radio.sw_crc_bytes = (uint8_t)msg.cB;
                            tinygs_radio.sw_crc_init = (uint16_t)msg.cI;
                            tinygs_radio.sw_crc_poly = (uint16_t)msg.cP;
                            tinygs_radio.sw_crc_xor = (uint16_t)msg.cF;
                            tinygs_radio.sw_crc_refin = msg.cRI;
                            tinygs_radio.sw_crc_refout = msg.cRO;

                            radio->setRxBoostedGainMode(true);
                        }
                    }

                    /* Parse filter (array — not handled by json.h descriptors) */
                    int filt_count = tinygs_parse_filter(
                        tinygs_radio.modem_conf, strlen(tinygs_radio.modem_conf),
                        tinygs_radio.filter, sizeof(tinygs_radio.filter));
                    tinygs_radio.filter_len = (filt_count > 0) ? filt_count : 0;
                    if (filt_count <= 0) tinygs_radio.filter[0] = 0;

                    /* Parse TLE base64 from modem_conf copy.
                     * "tle" key = active Doppler compensation
                     * "tlx" key = passive TLE (position display only, no Doppler) */
                    const char *tle_p = strstr(tinygs_radio.modem_conf, "\"tle\":\"");
                    const char *tlx_p = strstr(tinygs_radio.modem_conf, "\"tlx\":\"");
                    const char *tle_key = tle_p ? tle_p : tlx_p;
                    bool active_doppler = (tle_p != NULL);

                    if (tle_key) {
                        const char *b64_start = tle_key + 7; /* skip "tle":"  or "tlx":" (7 chars) */
                        const char *b64_end = strchr(b64_start, '"');
                        if (b64_end) {
                            size_t decoded_len = 0;
                            if (base64_decode((uint8_t *)tinygs_radio.tle,
                                              sizeof(tinygs_radio.tle),
                                              &decoded_len,
                                              (const uint8_t *)b64_start,
                                              b64_end - b64_start) == 0
                                && decoded_len >= 34
                                && decoded_len <= sizeof(tinygs_radio.tle)) {
                                tinygs_radio.tle_valid = true;
                                tinygs_radio.doppler_enabled = active_doppler;
                                tinygs_radio.freq_doppler = 0.0f;
                                LOG_INF("  TLE received (%zu bytes)%s",
                                        decoded_len, active_doppler ? " +Doppler" : "");
                            } else {
                                tinygs_radio.tle_valid = false;
                            }
                        }
                    } else {
                        tinygs_radio.tle_valid = false;
                        tinygs_radio.doppler_enabled = false;
                    }

                    /* Re-enable RX interrupt and start receiving */
                    radio->setPacketReceivedAction(lora_rx_callback);
                    radio->startReceive();

                    /* tle_tag: "TLE" = active Doppler (server sent "tle" key),
                     *          "TLX" = passive map data only (server sent "tlx"),
                     *          ""    = no orbit data at all. */
                    const char *tle_tag = tinygs_radio.tle_valid
                        ? (tinygs_radio.doppler_enabled ? " TLE" : " TLX")
                        : "";
                    if (strcmp(tinygs_radio.modem_mode, "FSK") == 0) {
                        LOG_INF("  → %s %.4fMHz FSK BR%.0f BW%.1f%s%s",
                                tinygs_radio.satellite,
                                (double)tinygs_radio.frequency,
                                (double)tinygs_radio.bitrate,
                                (double)tinygs_radio.bw,
                                tle_tag,
                                tinygs_radio.filter[0] ? " FLT" : "");
                    } else {
                        LOG_INF("  → %s %.4fMHz SF%d BW%.1f%s%s",
                                tinygs_radio.satellite,
                                (double)tinygs_radio.frequency,
                                tinygs_radio.sf,
                                (double)tinygs_radio.bw,
                                tle_tag,
                                tinygs_radio.filter[0] ? " FLT" : "");
                    }
                }
            } else if (strcmp(cmnd, "begin_lora") == 0) {
                /* Legacy array format: [freq,bw,sf,cr,sw,pwr,climit,pl,gain] */
                if (radio) {
                    radio->clearPacketReceivedAction();
                    if (lora_packet_received) lora_packet_received = false;
                    float vals[9] = {0};
                    const char *p = strchr((char *)rx_payload, '[');
                    if (p) {
                        p++;
                        for (int i = 0; i < 9 && *p && *p != ']'; i++) {
                            vals[i] = strtof(p, (char **)&p);
                            while (*p == ',' || *p == ' ') p++;
                        }
                        float freq = vals[0];
                        if (freq > 100.0f && freq < 1000.0f) {
                            tinygs_radio.frequency = freq;
                            radio->setFrequency(freq + tinygs_radio.freq_offset / 1e6f);
                            radio->setBandwidth(vals[1]); tinygs_radio.bw = vals[1];
                            radio->setSpreadingFactor((int)vals[2]); tinygs_radio.sf = (int)vals[2];
                            radio->setCodingRate((int)vals[3]); tinygs_radio.cr = (int)vals[3];
                            radio->setSyncWord((int)vals[4]);
                            radio->setPreambleLength((int)vals[7]);
                            radio->setRxBoostedGainMode(true);
                            strncpy(tinygs_radio.modem_mode, "LoRa", sizeof(tinygs_radio.modem_mode));
                            radio->setPacketReceivedAction(lora_rx_callback);
                            radio->startReceive();
                            LOG_INF("  begin_lora: %.4fMHz SF%d BW%.1f",
                                    (double)freq, (int)vals[2], (double)vals[1]);
                        }
                    }
                }
            } else if (strcmp(cmnd, "begin_fsk") == 0) {
                /* Legacy array format: [freq,br,fd,rxBw,pwr,pl,ook,len] */
                if (radio) {
                    radio->clearPacketReceivedAction();
                    if (lora_packet_received) lora_packet_received = false;
                    float vals[8] = {0};
                    const char *p = strchr((char *)rx_payload, '[');
                    if (p) {
                        p++;
                        for (int i = 0; i < 8 && *p && *p != ']'; i++) {
                            vals[i] = strtof(p, (char **)&p);
                            while (*p == ',' || *p == ' ') p++;
                        }
                        float freq = vals[0];
                        if (freq > 100.0f && freq < 1000.0f) {
                            tinygs_radio.frequency = freq;
                            tinygs_radio.bw = vals[3];
                            tinygs_radio.bitrate = vals[1];
                            tinygs_radio.freq_dev = vals[2];
                            tinygs_radio.ook = (int)vals[6];
                            tinygs_radio.fsk_len = (int)vals[7];
                            strncpy(tinygs_radio.modem_mode, "FSK", sizeof(tinygs_radio.modem_mode));
                            int16_t rc = radio->beginFSK(
                                freq + tinygs_radio.freq_offset / 1e6f,
                                vals[1] / 1000.0f, vals[2] / 1000.0f, vals[3],
                                (int)vals[4], (int)vals[5], LORA_TCXO_VOLTAGE);
                            if (rc == RADIOLIB_ERR_NONE) {
                                radio->setPacketReceivedAction(lora_rx_callback);
                                radio->setCRC(0);
                                if ((int)vals[7] > 0) radio->fixedPacketLengthMode((int)vals[7]);
                                radio->setRxBoostedGainMode(true);
                                radio->startReceive();
                                LOG_INF("  begin_fsk: %.4fMHz BR%.0f FD%.0f",
                                        (double)freq, (double)vals[1], (double)vals[2]);
                            } else {
                                LOG_ERR("  begin_fsk failed: %d (%s)", rc, radio_err_name(rc));
                            }
                        }
                    }
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
                /* Select satellite — payload is ["name", NORAD] */
                const char *p = strstr((char *)rx_payload, "\"");
                if (p) {
                    const char *end = strchr(p + 1, '"');
                    if (end && (end - p - 1) < (int)sizeof(tinygs_radio.satellite)) {
                        memcpy(tinygs_radio.satellite, p + 1, end - p - 1);
                        tinygs_radio.satellite[end - p - 1] = '\0';
                        /* Parse NORAD after the closing quote: ,"12345"] */
                        const char *comma = strchr(end, ',');
                        if (comma) {
                            tinygs_radio.norad = (uint32_t)atoi(comma + 1);
                        }
                        LOG_INF("  → satellite: %s NORAD=%u",
                                tinygs_radio.satellite, (unsigned)tinygs_radio.norad);
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
                /* Payload is the raw bytes to transmit on the current radio
                 * config. We advertise tx=false in welcome so the server
                 * normally won't send this, but handle it properly if it
                 * does arrive. Returning to RX after TX is best-effort. */
                if (radio && ret > 0) {
                    int16_t trc = radio->transmit((const uint8_t *)rx_payload, ret);
                    radio->startReceive();
                    LOG_INF("  → TX %zu bytes, rc=%d", (size_t)ret, trc);
                } else {
                    LOG_WRN("  → TX skipped (no radio or empty payload)");
                }
            } else if (strcmp(cmnd, "log") == 0) {
                LOG_INF("  → Server: %s", (char *)rx_payload);
            } else if (strcmp(cmnd, "sleep") == 0 || strcmp(cmnd, "siesta") == 0) {
                uint32_t secs = tinygs_parse_sleep((char *)rx_payload, ret);
                if (secs == 0) {
                    LOG_WRN("  → %s: invalid payload, ignoring", cmnd);
                } else {
                    LOG_INF("  → %s: %u seconds", cmnd, secs);
                    /* Put radio in sleep first — dominant current draw */
                    if (radio) radio->sleep();
                    /* Disconnect MQTT so the broker drops us cleanly rather
                     * than holding state through a missed keepalive. */
                    mqtt_disconnect(client);
                    k_msleep(200);
                    /* Block the main thread — Zephyr's tick idle handler
                     * lets the CPU enter low-power sleep. OpenThread will
                     * still service MAC frames on its own thread. */
                    k_sleep(K_SECONDS(secs));
                    /* Reboot on wake — cleanest re-init of radio + network
                     * (matches ESP32 deep-sleep-wake-then-reset behaviour). */
                    LOG_INF("  → wake — rebooting");
                    sys_reboot(SYS_REBOOT_COLD);
                }
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
                /* Server pushes a URL pointing at a new firmware image.
                 * On our hardware there is no network-OTA path: the
                 * Adafruit UF2 bootloader requires a physical USB mass-
                 * storage copy of a .uf2 file. We log the URL so the
                 * operator can fetch and flash it manually. A future
                 * MCUboot-based variant could fetch via CoAP over Thread
                 * and stage into slot1; that's a hardware change, not a
                 * code change in this firmware. */
                if (ret > 0) {
                    /* Payload is not guaranteed NUL-terminated — copy to bounded buf */
                    char url[128];
                    size_t n = ((size_t)ret < sizeof(url) - 1) ? (size_t)ret
                                                              : sizeof(url) - 1;
                    memcpy(url, rx_payload, n);
                    url[n] = '\0';
                    LOG_INF("  → update URL: %s", url);
                    LOG_INF("  → (UF2 bootloader: fetch + drag-and-drop to HT-n5262 to apply)");
                } else {
                    LOG_INF("  → update: (empty payload)");
                }
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
            } else if (strcmp(cmnd, "set_adv_prm") == 0) {
                /* Server pushes an advanced-params JSON blob. Store it opaquely;
                 * ESP32 parses it into ConfigManager but we have no concrete use
                 * for the fields yet — keep the raw string so get_adv_prm can
                 * echo it back. */
                size_t n = (ret < (int)sizeof(cfg_adv_prm) - 1) ? (size_t)ret
                                                                : sizeof(cfg_adv_prm) - 1;
                memcpy(cfg_adv_prm, rx_payload, n);
                cfg_adv_prm[n] = '\0';
                LOG_INF("  → set_adv_prm stored (%zu bytes)", n);
            } else if (strcmp(cmnd, "get_adv_prm") == 0) {
                /* Server asks us to echo our current adv_prm. Publish to
                 * tele/get_adv_prm (ESP32 uses the same topic for response). */
                tinygs_send_adv_prm(client, cfg_mqtt_user, cfg_station, cfg_adv_prm);
                LOG_INF("  → get_adv_prm responded");
            } else if (strcmp(cmnd, "remoteTune") == 0) {
                /* Newer server command — payload is a single number applied as
                 * an additional freq offset in Hz. */
                float off = strtof((char *)rx_payload, NULL);
                tinygs_radio.freq_offset = off;
                if (radio) {
                    radio->setFrequency(tinygs_radio.frequency +
                                        tinygs_radio.freq_offset / 1e6f);
                    radio->startReceive();
                }
                LOG_INF("  → remoteTune offset=%.0f Hz", (double)off);
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
            LOG_ERR("tls_credential_add failed: %d (%s)", rc, errno_name(rc));
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
        LOG_ERR("mqtt_connect() failed: %d (%s, errno=%d/%s)",
                ret, errno_name(ret), errno, errno_name(errno));
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
    LOG_INF("FATFS mount: %d (%s)", res, res < 0 ? errno_name(res) : "OK");

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

                    float lat = json_extract_float(cfg_buf, "\"lat\":", -999.0f);
                    if (lat >= -90.0f && lat <= 90.0f) tinygs_station_lat = lat;

                    float lon = json_extract_float(cfg_buf, "\"lon\":", -999.0f);
                    if (lon >= -180.0f && lon <= 180.0f) tinygs_station_lon = lon;

                    tinygs_station_alt = json_extract_float(cfg_buf, "\"alt\":", tinygs_station_alt);

                    json_extract_string(cfg_buf, "\"station\":\"", cfg_station, sizeof(cfg_station));
                    json_extract_string(cfg_buf, "\"mqtt_user\":\"", cfg_mqtt_user, sizeof(cfg_mqtt_user));
                    json_extract_string(cfg_buf, "\"mqtt_pass\":\"", cfg_mqtt_pass, sizeof(cfg_mqtt_pass));

                    int dt = json_extract_int(cfg_buf, "\"display_timeout\":", -1);
                    if (dt >= 0) tinygs_display_set_timeout((uint32_t)dt);
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
/* lora_packet_received moved to top — see forward declaration */

/* -------------------------------------------------------------------------- */
/* SNTP Time Sync                                                              */
/* -------------------------------------------------------------------------- */

/* Offset in microseconds: real_epoch_us = k_uptime_get_us() + sntp_epoch_offset_us.
 * Storing us (not s) removes the sub-second quantization at sync — matters for
 * usec_time which feeds the TLE positioner (~7 km/s LEO velocity). Using the
 * 64-bit k_uptime_get() (not the _32 variant) eliminates the 49.7-day rollover
 * on uptime_s that previously made epoch jump backwards after ~50 days. */
static int64_t sntp_epoch_offset_us = 0;
static bool time_synced = false;

int64_t get_utc_epoch_us(void)
{
    if (!time_synced) return 0;
    return k_uptime_get() * 1000LL + sntp_epoch_offset_us;
}

int64_t get_utc_epoch(void)
{
    if (!time_synced) return 0;
    return get_utc_epoch_us() / 1000000LL;
}

/* SNTP callback — called from OpenThread context */
static void sntp_response_handler(void *aContext, uint64_t aTime, otError aResult)
{
    if (aResult == OT_ERROR_NONE) {
        int64_t uptime_us = k_uptime_get() * 1000LL;
        int64_t aTime_us  = (int64_t)aTime * 1000000LL;
        sntp_epoch_offset_us = aTime_us - uptime_us;
        time_synced = true;
        LOG_INF("SNTP: synced, epoch=%llu", (unsigned long long)aTime);
    } else {
        LOG_WRN("SNTP: failed (%d %s)", (int)aResult, otThreadErrorToString(aResult));
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
        LOG_WRN("SNTP: query failed (%d %s)", (int)err, otThreadErrorToString(err));
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
    if (!tinygs_radio.tle_valid || !radio) {
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

    /* Get elevation and satellite map position */
    double elevation = 0.0, azimuth = 0.0;
    sat.elaz(obs, elevation, azimuth);

    /* Update satellite position for world map display (128x64 ESP32 coords) */
    double sat_lat = 0, sat_lon = 0;
    sat.latlon(sat_lat, sat_lon);
    tinygs_radio.sat_pos_x = (float)((180.0 + sat_lon) / 360.0 * 128.0);
    tinygs_radio.sat_pos_y = (float)((90.0 - sat_lat) / 180.0 * 64.0);

    if (elevation <= 0.0 || !tinygs_radio.doppler_enabled) {
        return; /* Below horizon or Doppler disabled (tlx) — position updated, no freq correction */
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

    /* T114 SX1262 uses TCXO on DIO3 at 1.8V — MUST be set or
     * the frequency reference is dead and all packets fail CRC */
    int state = radio->begin(TINYGS_DEFAULT_FREQ, 250.0, 10, 5, 0x12, 5, 8, 1.8);
    if (state == RADIOLIB_ERR_NONE) {
        LOG_INF("Radio: %s initialized (RadioLib)",
                DT_NODE_FULL_NAME(DT_NODELABEL(sx1262)));
    } else {
        LOG_ERR("Radio init failed: %d (%s)", state, radio_err_name(state));
        return;
    }

    /* begin() already set default params, just add boosted gain */
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
        LOG_ERR("startReceive failed: %d (%s)", state, radio_err_name(state));
    }
}

/*
 * Apply the modem config persisted by the last beginp. Called after
 * init_radio() — overrides the hardcoded defaults when NVS has a saved
 * blob. Subset of the full begine handler: applies the core radio
 * settings (mode/freq/bw/sf/cr/sw/pl/iIQ for LoRa; freq/br/fd/bw/pl/sync
 * for FSK). The server will send a fresh `begine` on next connect which
 * will fully reconfigure including filter + TLE, so we don't duplicate
 * that logic here.
 */
static void apply_saved_modem_conf(void)
{
    if (!radio) return;
    char *conf = tinygs_radio.modem_conf;
    if (!conf || conf[0] == '\0' || strcmp(conf, "{}") == 0) {
        LOG_INF("No saved modem_conf — using defaults");
        return;
    }

    /* tinygs_parse_begine modifies the buffer in place (strtok-style) — we
     * only want to inspect what we saved, not mangle it. Parse a copy. */
    static char scratch[sizeof(tinygs_radio.modem_conf)];
    size_t n = strlen(conf);
    memcpy(scratch, conf, n + 1);

    struct tinygs_begine_msg msg;
    int64_t rc = tinygs_parse_begine(scratch, n, &msg);
    if (rc < 0 || !msg.mode) {
        LOG_WRN("Saved modem_conf unparsable — using defaults");
        return;
    }

    bool is_fsk = (strcmp(msg.mode, "FSK") == 0);
    float freq = tinygs_begine_get_freq(&msg);
    float bw   = tinygs_begine_get_bw(&msg);
    if (is_fsk) {
        float br = tinygs_begine_get_br(&msg);
        float fd = tinygs_begine_get_fd(&msg);
        int16_t src = radio->beginFSK(freq, br, fd, bw,
                                      msg.pwr, msg.pl, 1.8f);
        if (src == RADIOLIB_ERR_NONE) {
            tinygs_radio.frequency   = freq;
            tinygs_radio.bw          = bw;
            tinygs_radio.bitrate     = br;
            tinygs_radio.freq_dev    = fd;
            strncpy(tinygs_radio.modem_mode, "FSK",
                    sizeof(tinygs_radio.modem_mode));
            LOG_INF("Applied saved modem_conf: FSK %.4f MHz br=%.3f fd=%.3f bw=%.1f",
                    (double)freq, (double)br, (double)fd, (double)bw);
        } else {
            LOG_WRN("Saved FSK config rejected (%d), using defaults", src);
        }
    } else {
        int16_t src = radio->begin(freq, bw, msg.sf, msg.cr,
                                   msg.sw, msg.pwr, msg.pl, 1.8f);
        if (src == RADIOLIB_ERR_NONE) {
            radio->invertIQ(msg.iIQ);
            tinygs_radio.frequency = freq;
            tinygs_radio.bw        = bw;
            tinygs_radio.sf        = msg.sf;
            tinygs_radio.cr        = msg.cr;
            tinygs_radio.iIQ       = msg.iIQ;
            strncpy(tinygs_radio.modem_mode, "LoRa",
                    sizeof(tinygs_radio.modem_mode));
            LOG_INF("Applied saved modem_conf: LoRa %.4f MHz SF%d CR%d BW%.1f",
                    (double)freq, msg.sf, msg.cr, (double)bw);
        } else {
            LOG_WRN("Saved LoRa config rejected (%d), using defaults", src);
        }
    }
    /* Satellite name + NORAD for welcome/status continuity */
    if (msg.sat) {
        strncpy(tinygs_radio.satellite, msg.sat,
                sizeof(tinygs_radio.satellite) - 1);
        tinygs_radio.satellite[sizeof(tinygs_radio.satellite) - 1] = '\0';
    }
    tinygs_radio.norad = (uint32_t)msg.NORAD;
    radio->startReceive();
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

    /* Store last-packet metrics for status payload */
    tinygs_radio.last_rssi = rssi;
    tinygs_radio.last_snr = snr;
    tinygs_radio.last_freq_err = freq_err;
    tinygs_radio.last_crc_error = (state == RADIOLIB_ERR_CRC_MISMATCH);

    if (state == RADIOLIB_ERR_NONE) {
        lora_rx_count++;
        /* Flash LED1 white on packet RX */
        neopixel_set(NEO_OFF,  NEO_WHITE); /* LED1 flash on LoRa RX */
        LOG_INF("LoRa RX: %u bytes, RSSI=%.1f, SNR=%.1f, FreqErr=%.1f",
                (unsigned)len, (double)rssi, (double)snr, (double)freq_err);

        /* FSK framing post-processing (AX.25, PN9 descramble).
         * Applied before filter and CRC checks, matching ESP32 order. */
        bool frame_error = false;
        if (strcmp(tinygs_radio.modem_mode, "FSK") == 0) {
            if (tinygs_radio.fsk_framing == 1 || tinygs_radio.fsk_framing == 3) {
                /* AX.25 NRZS decode (framing=1) or scrambled AX.25 (framing=3) */
                /* Need FSK sync word from radio state for prepending */
                uint8_t fsw_buf[8];
                int fsw_len = tinygs_parse_fsw(
                    tinygs_radio.modem_conf, strlen(tinygs_radio.modem_conf),
                    fsw_buf, sizeof(fsw_buf));

                /* TODO(tests): temporary diagnostic — logs the raw pre-decode
                 * FSK bytes alongside the decoded AX.25 frame so we can build
                 * a unit test for bitcode_nrz2ax25 the next time a real packet
                 * lands (SAMSAT, Colibri-S, Norby-2 etc.). Remove once a
                 * regression test exists in tests/json_parser. */
                LOG_HEXDUMP_INF(data, (len < 64 ? len : 64),
                                "AX.25 pre-decode raw FSK bytes");
                LOG_INF("AX.25 pre-decode: len=%u, fsw_len=%d, framing=%d",
                        (unsigned)len, fsw_len, tinygs_radio.fsk_framing);

                uint8_t ax25[256];
                size_t ax25_len = 0;
                int rc = bitcode_nrz2ax25(data, len, fsw_buf, fsw_len,
                                          tinygs_radio.fsk_framing,
                                          ax25, &ax25_len, sizeof(ax25));
                if (rc == 0 && ax25_len > 0) {
                    memcpy(data, ax25, ax25_len);
                    len = ax25_len;
                    LOG_INF("AX.25 decoded: %u bytes", (unsigned)len);
                } else {
                    frame_error = true;
                    LOG_WRN("AX.25 frame error");
                }
            } else if (tinygs_radio.fsk_framing == 2) {
                /* PN9 descramble */
                uint8_t descrambled[256];
                bitcode_pn9(data, len, descrambled);
                memcpy(data, descrambled, len);
            }
        }

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

        /* Software CRC check (FSK mode with cSw enabled).
         * The last crc_bytes of the packet contain the CRC.
         * Matches ESP32 Radio.cpp line 631-677 logic. */
        bool sw_crc_fail = false;
        if (tinygs_radio.sw_crc_enabled && tinygs_radio.sw_crc_bytes > 0 &&
            len > tinygs_radio.sw_crc_bytes) {
            size_t data_len = len - tinygs_radio.sw_crc_bytes;
            uint16_t crc = tinygs_radio.sw_crc_init;
            uint16_t poly = tinygs_radio.sw_crc_poly;
            for (size_t i = 0; i < data_len; i++) {
                uint8_t b = tinygs_radio.sw_crc_refin ?
                    __RBIT((uint32_t)data[i]) >> 24 : data[i];
                if (tinygs_radio.sw_crc_bytes == 2) {
                    crc ^= (uint16_t)b << 8;
                    for (int j = 0; j < 8; j++) {
                        crc = (crc & 0x8000) ? (crc << 1) ^ poly : crc << 1;
                    }
                } else {
                    crc ^= b;
                    for (int j = 0; j < 8; j++) {
                        crc = (crc & 0x80) ? (crc << 1) ^ poly : crc << 1;
                    }
                }
            }
            crc ^= tinygs_radio.sw_crc_xor;
            if (tinygs_radio.sw_crc_refout) {
                crc = (uint16_t)(__RBIT((uint32_t)crc) >> (32 - tinygs_radio.sw_crc_bytes * 8));
            }
            crc &= (tinygs_radio.sw_crc_bytes == 1) ? 0xFF : 0xFFFF;

            /* Extract CRC from packet tail */
            uint16_t pkt_crc;
            if (tinygs_radio.sw_crc_refin) {
                uint8_t b0 = __RBIT((uint32_t)data[len - 2]) >> 24;
                uint8_t b1 = __RBIT((uint32_t)data[len - 1]) >> 24;
                pkt_crc = (uint16_t)b0 << 8 | b1;
            } else {
                pkt_crc = (uint16_t)data[len - 2] << 8 | data[len - 1];
            }

            if (crc != pkt_crc) {
                sw_crc_fail = true;
                LOG_WRN("FSK SW CRC fail: calc=%04x pkt=%04x", crc, pkt_crc);
            }
        }

        /* Notify display of packet reception */
        tinygs_display_packet_rx(rssi, snr);

        /* Publish via MQTT if connected */
        if (app_state == STATE_MQTT_CONNECTED) {
            if (frame_error) {
                static const uint8_t err_frame[] = "Frame error!";
                tinygs_send_rx(&mqtt_client, cfg_mqtt_user, cfg_station,
                               err_frame, sizeof(err_frame) - 1, rssi, snr, freq_err, true);
            } else if (sw_crc_fail) {
                static const uint8_t err_crc[] = "Error_CRC";
                tinygs_send_rx(&mqtt_client, cfg_mqtt_user, cfg_station,
                               err_crc, sizeof(err_crc) - 1, rssi, snr, freq_err, true);
            } else {
                tinygs_send_rx(&mqtt_client, cfg_mqtt_user, cfg_station,
                               data, len, rssi, snr, freq_err, false);
            }
        }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        LOG_WRN("LoRa RX: CRC error, %u bytes, RSSI=%.1f, SNR=%.1f, FreqErr=%.1f",
                (unsigned)len, (double)rssi, (double)snr, (double)freq_err);
        /* Dump first 16 bytes for diagnosis */
        LOG_HEXDUMP_DBG(data, len < 16 ? len : 16, "CRC err data");

        /* Send CRC error packets to server like ESP32 does.
         * With filter active (filter[0] != 0), ESP32 drops CRC errors.
         * With no filter, it sends "Error_CRC" as the payload. */
        if (tinygs_radio.filter[0] == 0 && app_state == STATE_MQTT_CONNECTED) {
            static const uint8_t err_crc[] = "Error_CRC";
            tinygs_send_rx(&mqtt_client, cfg_mqtt_user, cfg_station,
                           err_crc, sizeof(err_crc) - 1, rssi, snr, freq_err, true);
        }
    } else {
        LOG_ERR("LoRa readData failed: %d (%s)", state, radio_err_name(state));
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
        uint32_t vectactive = crash_icsr & 0x1FF;
        int irq_num = (int)vectactive - 16;
        unsigned r = crash_reason & 0xFFFF;
        static const char *reason_names[] = {
            "CPU_EXCEPTION", "SPURIOUS_IRQ", "STACK_CHK_FAIL",
            "KERNEL_OOPS",   "KERNEL_PANIC"
        };
        const char *reason_str = (r < ARRAY_SIZE(reason_names)) ? reason_names[r] : "ARCH_SPECIFIC";
        /* nRF52840 external IRQ number → peripheral name */
        static const char *irq_names[] = {
            "POWER_CLOCK","RADIO","UARTE0","SPI0","SPI1","NFCT","GPIOTE","SAADC",
            "TIMER0","TIMER1","TIMER2","RTC0","TEMP","RNG","ECB","CCM_AAR",
            "WDT","RTC1","QDEC","COMP","SWI0","SWI1","SWI2","SWI3","SWI4","SWI5",
            "TIMER3","TIMER4","PWM0","PDM","res30","res31","MWU","PWM1","PWM2",
            "SPI2","RTC2","I2S","FPU","USBD","UARTE1","QSPI","CRYPTOCELL"
        };
        const char *irq_str = (irq_num >= 0 && irq_num < (int)ARRAY_SIZE(irq_names))
                               ? irq_names[irq_num] : "?";
        LOG_ERR("*** PREVIOUS CRASH: thread='%s' reason=%u(%s) PC=0x%08x LR=0x%08x ICSR=0x%08x IRQ=%d(%s) ***",
                crash_thread[0] ? crash_thread : "?",
                r, reason_str,
                (unsigned)crash_pc, (unsigned)crash_lr,
                (unsigned)crash_icsr, irq_num, irq_str);
        LOG_ERR("Use: arm-zephyr-eabi-addr2line -e build/zephyr/zephyr.elf 0x%08x",
                (unsigned)crash_pc);
        crash_reason = 0; /* Clear so we don't report again */
    }

    log_heap_usage("boot");
    init_openthread();

#if defined(CONFIG_IOT_LOG)
    {
        iot_log_config_t log_cfg = IOT_LOG_CONFIG_DEFAULT();
        log_cfg.device_name = "TinyGS-nRF52";
        log_cfg.level = IOT_LOG_DEBUG;
        log_cfg.always_on = false;
        iot_log_init(&log_cfg);
    }
#endif

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
    apply_saved_modem_conf();

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
#if defined(CONFIG_IOT_LOG)
                /* Trigger deferred multicast join now that Thread is attached */
                iot_log_poll();
#endif
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
                        LOG_DBG("mqtt_live: %d (%s)", live_ret, errno_name(live_ret));
                    }
                    /* Zephyr mqtt_client doesn't auto-disconnect on missed
                     * PINGRESPs — it just increments unacked_ping forever.
                     * Force a clean reconnect after 2 missed responses so
                     * the server drops our stale session rather than
                     * waiting for the 600 s watchdog (which we saw fire
                     * 4× overnight from NAT64 idle hangs).
                     * PubSubClient on ESP32 does this for free. */
                    if (mqtt_client.unacked_ping >= TINYGS_MQTT_UNACKED_PING_MAX) {
                        LOG_WRN("MQTT: %d unacked PINGREQs — forcing reconnect",
                                mqtt_client.unacked_ping);
                        mqtt_disconnect(&mqtt_client);
                        app_state = STATE_MQTT_CONNECT;
                        break;
                    }
                }

                /* Check for LoRa packet reception */
                lora_check_rx();

                /* Update display (~every 100ms from poll timeout) */
                tinygs_display_update();

#if defined(CONFIG_IOT_LOG)
                /* Check for remote log listener beacons */
                iot_log_poll();
#endif

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
