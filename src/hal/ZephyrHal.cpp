#include "ZephyrHal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ZephyrHal, LOG_LEVEL_DBG);

ZephyrHal::ZephyrHal(const struct device* spi_dev, struct spi_config* spi_cfg)
  : RadioLibHal(HAL_PIN_INPUT, HAL_PIN_OUTPUT, HAL_PIN_LOW, HAL_PIN_HIGH, HAL_PIN_RISING, HAL_PIN_FALLING),
    _spi_dev(spi_dev), _spi_cfg(spi_cfg) {
    
    for (int i = 0; i < MAX_HAL_PINS; i++) {
        _pins[i] = nullptr;
    }
}

ZephyrHal::~ZephyrHal() {
}

uint32_t ZephyrHal::addPin(const struct gpio_dt_spec* dt_spec) {
    if (_pin_count >= MAX_HAL_PINS) {
        LOG_ERR("Max HAL pins exceeded!");
        return 0xFFFFFFFF; // Error
    }
    
    _pins[_pin_count] = dt_spec;
    
    if (!gpio_is_ready_dt(dt_spec)) {
        LOG_ERR("GPIO %s is not ready", dt_spec->port->name);
    }
    
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

    zephyr_gpio_dir_t dir = (mode == HAL_PIN_OUTPUT) ? GPIO_OUTPUT : GPIO_INPUT;
    gpio_pin_configure_dt(dt, dir);
}

void ZephyrHal::digitalWrite(uint32_t pin, uint32_t value) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return;

    gpio_pin_set_dt(dt, value == HAL_PIN_HIGH ? 1 : 0);
}

uint32_t ZephyrHal::digitalRead(uint32_t pin) {
    const struct gpio_dt_spec* dt = getGpio(pin);
    if (!dt) return HAL_PIN_LOW;

    return gpio_pin_get_dt(dt) > 0 ? HAL_PIN_HIGH : HAL_PIN_LOW;
}

// Global mapping for ISRs (since Zephyr ISRs need user_data, but RadioLib expects void(*)(void))
// We'll store a static array of callbacks. Not thread-safe if multiple radios, but works for one.
static void (*isr_callbacks[MAX_HAL_PINS])(void) = {nullptr};

static void zephyr_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    // Find which logical pin triggered by matching the cb pointer
    // In a real implementation, we'd calculate offset.
    // For now, let's just run all active matching callbacks.
    for (int i = 0; i < MAX_HAL_PINS; i++) {
        if (isr_callbacks[i] != nullptr) {
            // Check if this callback matches the pin that fired
            // (Simplified for PoC)
            isr_callbacks[i](); 
        }
    }
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

    gpio_pin_interrupt_configure_dt(dt, flags);
    gpio_init_callback(&_callbacks[interruptNum], zephyr_gpio_isr, BIT(dt->pin));
    gpio_add_callback(dt->port, &_callbacks[interruptNum]);
    
    isr_callbacks[interruptNum] = interruptCb;
}

void ZephyrHal::detachInterrupt(uint32_t interruptNum) {
    const struct gpio_dt_spec* dt = getGpio(interruptNum);
    if (!dt) return;

    gpio_pin_interrupt_configure_dt(dt, GPIO_INT_DISABLE);
    gpio_remove_callback(dt->port, &_callbacks[interruptNum]);
    isr_callbacks[interruptNum] = nullptr;
}

void ZephyrHal::delay(unsigned long ms) {
    k_msleep(ms);
}

void ZephyrHal::delayMicroseconds(unsigned long us) {
    k_busy_wait(us);
}

unsigned long ZephyrHal::millis() {
    return k_uptime_get_32();
}

unsigned long ZephyrHal::micros() {
    return k_uptime_get_32() * 1000;
}

void ZephyrHal::spiBegin() {
    // Zephyr handles SPI init automatically at boot
}

void ZephyrHal::spiBeginTransaction() {
    // Zephyr uses the spi_config struct per transaction
}

void ZephyrHal::spiTransfer(uint8_t* out, size_t len, uint8_t* in) {
    const struct spi_buf tx_buf = { .buf = out, .len = len };
    const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
    
    struct spi_buf rx_buf = { .buf = in, .len = len };
    const struct spi_buf_set rx = { .buffers = &rx_buf, .count = 1 };

    // If out is NULL, Zephyr will just clock out dummy bytes (0x00).
    // If in is NULL, Zephyr will discard incoming bytes.
    int ret = spi_transceive(_spi_dev, _spi_cfg, 
                             out ? &tx : nullptr, 
                             in ? &rx : nullptr);
    if (ret != 0) {
        LOG_ERR("SPI transfer failed: %d", ret);
    }
}

void ZephyrHal::spiEndTransaction() {
}

void ZephyrHal::spiEnd() {
}
