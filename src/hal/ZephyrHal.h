#ifndef ZEPHYR_HAL_H
#define ZEPHYR_HAL_H

#include <RadioLib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

/* Defines to map RadioLib pin states to Zephyr */
#define HAL_PIN_INPUT  1
#define HAL_PIN_OUTPUT 2
#define HAL_PIN_LOW    0
#define HAL_PIN_HIGH   1
#define HAL_PIN_RISING 1
#define HAL_PIN_FALLING 2

/* Maximum number of pins we support mapping */
#define MAX_HAL_PINS 10

class ZephyrHal : public RadioLibHal {
  public:
    ZephyrHal(const struct device* spi_dev, struct spi_config* spi_cfg);
    ~ZephyrHal();

    /* Add a pin to the HAL mapping. Returns the logical pin ID to pass to RadioLib. */
    uint32_t addPin(const struct gpio_dt_spec* dt_spec);

    /* GPIO methods */
    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    
    /* Interrupts */
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;
    
    /* Timing */
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    
    /* GPIO extra */
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;

    /* Scheduling */
    void yield() override;

    /* SPI */
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

  private:
    const struct device* _spi_dev;
    struct spi_config _spi_cfg; // Store by value to allow local modification

    /* Logical pin to Zephyr gpio_dt_spec mapping */
    const struct gpio_dt_spec* _pins[MAX_HAL_PINS];
    struct gpio_callback _callbacks[MAX_HAL_PINS];
    uint32_t _pin_count = 0;

    const struct gpio_dt_spec* getGpio(uint32_t pin);
    
    // To allow static ISR to find the instance and pin
    static ZephyrHal* _instance;
    friend void zephyr_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins);
};

// Prototype for friend (not static in header to match friend declaration)
void zephyr_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins);

#endif // ZEPHYR_HAL_H
