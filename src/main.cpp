#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

LOG_MODULE_REGISTER(tinygs_nrf52, LOG_LEVEL_DBG);

/* The mqtt client struct */
// static struct mqtt_client client_ctx;

/* MQTT Broker details */
#define SERVER_ADDR "mqtt.tinygs.com"
#define SERVER_PORT 8883
#define APP_CONNECT_TIMEOUT_MS 2000
#define APP_SLEEP_MSECS 500

/* Buffers for MQTT client */
// static uint8_t rx_buffer[256];
// static uint8_t tx_buffer[256];
// static uint8_t payload_buf[256];

/**
 * OpenThread State Change Callback
 */
static void ot_state_changed_handler(otChangedFlags flags, struct openthread_context *ot_context, void *user_data)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        switch (otThreadGetDeviceRole(ot_context->instance)) {
        case OT_DEVICE_ROLE_CHILD:
            LOG_INF("OpenThread Role: Child (Connected)");
            break;
        case OT_DEVICE_ROLE_ROUTER:
            LOG_INF("OpenThread Role: Router (Connected)");
            break;
        case OT_DEVICE_ROLE_LEADER:
            LOG_INF("OpenThread Role: Leader (Connected)");
            break;
        case OT_DEVICE_ROLE_DETACHED:
            LOG_INF("OpenThread Role: Detached");
            break;
        case OT_DEVICE_ROLE_DISABLED:
            LOG_INF("OpenThread Role: Disabled");
            break;
        }
    }
}

static struct openthread_state_changed_cb ot_state_cb = {
    .state_changed_cb = ot_state_changed_handler,
};

/**
 * Initialize OpenThread Network
 */
static void init_openthread(void)
{
    LOG_INF("Initializing OpenThread...");
    openthread_state_changed_cb_register(openthread_get_default_context(), &ot_state_cb);
    openthread_start(openthread_get_default_context());
}

/**
 * Main Application Loop
 */
int main(void)
{
    LOG_INF("TinyGS nRF52 Port - Phase 1: TLS over Thread PoC");
    LOG_INF("Board: Heltec T114");

    init_openthread();

    /* The main loop will eventually handle the state machine for MQTT keep-alives 
       and checking for LoRa packets. For now, we just sleep to let the OpenThread 
       stack connect to the Border Router in the background. */
    while (1) {
        k_msleep(APP_SLEEP_MSECS);
    }

    return 0;
}
