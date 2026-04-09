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
    void delay(unsigned long ms) override;
    void delayMicroseconds(unsigned long us) override;
    unsigned long millis() override;
    unsigned long micros() override;
    
    /* SPI */
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

  private:
    const struct device* _spi_dev;
    struct spi_config* _spi_cfg;

    /* Logical pin to Zephyr gpio_dt_spec mapping */
    const struct gpio_dt_spec* _pins[MAX_HAL_PINS];
    struct gpio_callback _callbacks[MAX_HAL_PINS];
    uint32_t _pin_count = 0;

    const struct gpio_dt_spec* getGpio(uint32_t pin);
};

#endif // ZEPHYR_HAL_H
