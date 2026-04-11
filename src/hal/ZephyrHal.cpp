#include "ZephyrHal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ZephyrHal, LOG_LEVEL_DBG);

/*
 * GPIO ISR — recovers the per-pin irq context via CONTAINER_OF,
 * then calls the RadioLib callback. No global singleton needed.
 */
static void zephyr_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    struct zephyr_hal_pin_irq *irq = CONTAINER_OF(cb, struct zephyr_hal_pin_irq, cb);
    if (irq->fn != nullptr) {
        irq->fn();
    }
}

ZephyrHal::ZephyrHal(const struct device* spi_dev, struct spi_config* spi_cfg)
  : RadioLibHal(HAL_PIN_INPUT, HAL_PIN_OUTPUT, HAL_PIN_LOW, HAL_PIN_HIGH, HAL_PIN_RISING, HAL_PIN_FALLING),
    _spi_dev(spi_dev) {

    /* Clone the SPI config so we can strip CS — RadioLib manages CS
     * manually via its own digitalWrite calls. */
    _spi_cfg = *spi_cfg;
    _spi_cfg.cs.gpio.port = nullptr;

    for (int i = 0; i < MAX_HAL_PINS; i++) {
        _pins[i] = nullptr;
        _irqs[i].fn = nullptr;
    }
}

ZephyrHal::~ZephyrHal() {
}

uint32_t ZephyrHal::addPin(const struct gpio_dt_spec* dt_spec) {
    if (_pin_count >= MAX_HAL_PINS) {
        LOG_ERR("Max HAL pins exceeded!");
        return 0xFFFFFFFF;
    }

    if (!gpio_is_ready_dt(dt_spec)) {
        LOG_ERR("GPIO %s pin %d is not ready — rejecting", dt_spec->port->name, dt_spec->pin);
        return 0xFFFFFFFF;
    }

    _pins[_pin_count] = dt_spec;
    return _pin_count++;
}

const struct gpio_dt_spec* ZephyrHal::getGpio(uint32_t pin) {
    if (pin >= _pin_count) {
        return nullptr;
    }
    return _pins[pin];
}

void ZephyrHal::pinMode(uint32_t pin, uint32_t mode) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return;

    /*
     * Use gpio_pin_configure (not _dt) to avoid applying DTS active-low flags.
     * RadioLib expects raw physical pin levels — it handles CS/reset polarity
     * internally. Applying GPIO_ACTIVE_LOW would invert the logic and break
     * SPI chip select and reset signaling.
     */
    gpio_flags_t dir = (mode == HAL_PIN_OUTPUT) ? GPIO_OUTPUT : GPIO_INPUT;
    gpio_pin_configure(dt->port, dt->pin, dir);
}

void ZephyrHal::digitalWrite(uint32_t pin, uint32_t value) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return;

    /* Raw physical level — RadioLib manages polarity internally */
    gpio_pin_set_raw(dt->port, dt->pin, value == HAL_PIN_HIGH ? 1 : 0);
}

uint32_t ZephyrHal::digitalRead(uint32_t pin) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return HAL_PIN_LOW;

    /* Raw physical level — RadioLib manages polarity internally */
    return gpio_pin_get_raw(dt->port, dt->pin) > 0 ? HAL_PIN_HIGH : HAL_PIN_LOW;
}

void ZephyrHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) {
    const struct gpio_dt_spec* dt = getGpio(interruptNum);
    if (!dt) return;

    gpio_flags_t flags = 0;
    if (mode == HAL_PIN_RISING) {
        flags = GPIO_INT_EDGE_RISING;
    } else if (mode == HAL_PIN_FALLING) {
        flags = GPIO_INT_EDGE_FALLING;
    } else {
        flags = GPIO_INT_EDGE_BOTH;
    }

    gpio_pin_interrupt_configure(dt->port, dt->pin, flags);
    gpio_init_callback(&_irqs[interruptNum].cb, zephyr_gpio_isr, BIT(dt->pin));
    gpio_add_callback(dt->port, &_irqs[interruptNum].cb);

    _irqs[interruptNum].fn = interruptCb;
}

void ZephyrHal::detachInterrupt(uint32_t interruptNum) {
    const struct gpio_dt_spec* dt = getGpio(interruptNum);
    if (!dt) return;

    gpio_pin_interrupt_configure(dt->port, dt->pin, GPIO_INT_DISABLE);
    gpio_remove_callback(dt->port, &_irqs[interruptNum].cb);
    _irqs[interruptNum].fn = nullptr;
}

void ZephyrHal::delay(RadioLibTime_t ms) {
    k_msleep(ms);
}

void ZephyrHal::delayMicroseconds(RadioLibTime_t us) {
    k_busy_wait(us);
}

RadioLibTime_t ZephyrHal::millis() {
    return k_uptime_get_32();
}

RadioLibTime_t ZephyrHal::micros() {
    return (RadioLibTime_t)k_ticks_to_us_near64(k_uptime_ticks());
}

long ZephyrHal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return 0;

    RadioLibTime_t start = micros();
    while (digitalRead(pin) != state) {
        if (micros() - start > timeout) return 0;
    }

    RadioLibTime_t pulse_start = micros();
    while (digitalRead(pin) == state) {
        if (micros() - pulse_start > timeout) return 0;
    }

    return (long)(micros() - pulse_start);
}

void ZephyrHal::yield() {
    k_yield();
}

void ZephyrHal::spiBegin() {
}

void ZephyrHal::spiBeginTransaction() {
}

void ZephyrHal::spiTransfer(uint8_t* out, size_t len, uint8_t* in) {
    /* Fixed-size dummy buffers for unused direction — RadioLib SPI
     * transfers are always small (register reads/writes ≤ 256 bytes). */
    static uint8_t dummy_tx[256];
    static uint8_t dummy_rx[256];

    if (len > sizeof(dummy_tx)) {
        LOG_ERR("SPI transfer too large: %zu > %zu", len, sizeof(dummy_tx));
        return;
    }

    if (!out) {
        memset(dummy_tx, 0x00, len);
        out = dummy_tx;
    }
    if (!in) {
        in = dummy_rx;
    }

    const struct spi_buf tx_buf = { .buf = out, .len = len };
    const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };

    struct spi_buf rx_buf = { .buf = in, .len = len };
    const struct spi_buf_set rx = { .buffers = &rx_buf, .count = 1 };

    int ret = spi_transceive(_spi_dev, &_spi_cfg, &tx, &rx);
    if (ret != 0) {
        LOG_ERR("SPI transfer failed: %d (len=%u)", ret, (unsigned)len);
    }

    if (IS_ENABLED(CONFIG_LOG) && len <= 16) {
        LOG_HEXDUMP_DBG(out, len, "SPI TX:");
        LOG_HEXDUMP_DBG(in, len, "SPI RX:");
    }
}

void ZephyrHal::spiEndTransaction() {
}

void ZephyrHal::spiEnd() {
}
