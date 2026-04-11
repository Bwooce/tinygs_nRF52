#ifndef ZEPHYR_HAL_H
#define ZEPHYR_HAL_H

/**
 * @file ZephyrHal.h
 * @brief RadioLib hardware abstraction layer for Zephyr RTOS.
 *
 * Bridges RadioLib's platform-independent C++ API to Zephyr's GPIO and SPI
 * drivers. Designed for nRF52840 but should work on any Zephyr-supported SoC
 * with SPI and GPIO peripherals.
 *
 * @par Key design decisions:
 * - **Raw GPIO levels**: Uses gpio_pin_set_raw/gpio_pin_get_raw to bypass
 *   DTS GPIO_ACTIVE_LOW flags. RadioLib manages CS/reset/DIO polarity
 *   internally; applying active-low inversion would break signaling.
 * - **Manual CS**: The SPI chip select is stripped from spi_config and
 *   delegated to RadioLib's own digitalWrite calls.
 * - **Multi-instance safe**: Interrupt dispatch uses CONTAINER_OF on
 *   per-pin irq structs — no global singleton required. Uses Zephyr's
 *   native NULL-buffer support for dummy SPI transfers to avoid race conditions
 *   and save RAM.
 * - **Pin mapping**: Zephyr gpio_dt_spec pointers are registered via addPin()
 *   and accessed by logical index. RadioLib sees sequential pin IDs (0, 1, 2...).
 *
 * @par Tested with:
 * - SX1262 on Heltec Mesh Node T114 (nRF52840)
 * - nRF Connect SDK v3.5.99-ncs1 / Zephyr RTOS
 */

#include <RadioLib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

/** @brief RadioLib GPIO direction: input */
#define HAL_PIN_INPUT  1
/** @brief RadioLib GPIO direction: output */
#define HAL_PIN_OUTPUT 2
/** @brief RadioLib GPIO level: low */
#define HAL_PIN_LOW    0
/** @brief RadioLib GPIO level: high */
#define HAL_PIN_HIGH   1
/** @brief RadioLib interrupt edge: rising */
#define HAL_PIN_RISING 1
/** @brief RadioLib interrupt edge: falling */
#define HAL_PIN_FALLING 2

/** @brief Maximum number of GPIO pins that can be registered with the HAL */
#define MAX_HAL_PINS 10

/**
 * @brief Per-pin interrupt context.
 *
 * Embeds the Zephyr gpio_callback and the RadioLib callback pointer so the
 * ISR can dispatch without a global singleton. The ISR uses CONTAINER_OF
 * to recover this struct from the gpio_callback pointer.
 */
struct zephyr_hal_pin_irq {
    struct gpio_callback cb;  /**< Zephyr GPIO callback struct */
    void (*fn)(void);         /**< RadioLib interrupt callback */
};

/**
 * @brief RadioLib HAL implementation for Zephyr RTOS.
 *
 * Implements all required RadioLibHal virtual methods: GPIO, SPI, timing,
 * and interrupts. Multiple instances are supported for multi-radio boards.
 */
class ZephyrHal : public RadioLibHal {
  public:
    /**
     * @brief Construct a new ZephyrHal.
     * @param spi_dev Zephyr SPI device (from DEVICE_DT_GET).
     * @param spi_cfg SPI configuration. CS is cloned and stripped internally;
     *                RadioLib manages CS via its own digitalWrite calls.
     */
    ZephyrHal(const struct device* spi_dev, struct spi_config* spi_cfg);
    ~ZephyrHal();

    /**
     * @brief Register a GPIO pin with the HAL.
     * @param dt_spec Pointer to a Zephyr gpio_dt_spec (must remain valid for
     *                the lifetime of the HAL — typically a static DTS macro).
     * @return Logical pin ID (0, 1, 2...) to pass to RadioLib, or 0xFFFFFFFF
     *         on error (max pins exceeded or GPIO not ready).
     */
    uint32_t addPin(const struct gpio_dt_spec* dt_spec);

    /** @name GPIO Methods */
    ///@{
    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    ///@}

    /** @name Interrupt Methods */
    ///@{
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;
    ///@}

    /** @name Timing Methods */
    ///@{
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    ///@}

    /**
     * @brief Measure pulse width on a pin.
     * @note Busy-waits on digitalRead. SX126x does not use this method.
     */
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;

    /** @brief Yield the current thread (calls k_yield). */
    void yield() override;

    /** @name SPI Methods
     * spiBegin/spiEnd are no-ops — SPI is initialized by Zephyr DTS.
     * spiBeginTransaction/spiEndTransaction are no-ops — CS handled by RadioLib.
     */
    ///@{
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;
    ///@}

  private:
    const struct device* _spi_dev;
    struct spi_config _spi_cfg;

    const struct gpio_dt_spec* _pins[MAX_HAL_PINS];
    struct zephyr_hal_pin_irq _irqs[MAX_HAL_PINS];
    uint32_t _pin_count = 0;

    /**
     * @brief Look up a GPIO spec by logical pin ID.
     * @return Pointer to gpio_dt_spec, or nullptr if pin ID is out of range.
     */
    const struct gpio_dt_spec* getGpio(uint32_t pin);
};

#endif // ZEPHYR_HAL_H
