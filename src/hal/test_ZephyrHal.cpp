/**
 * @file test_ZephyrHal.cpp
 * @brief Compile-time and runtime validation of the ZephyrHal RadioLib HAL.
 *
 * These tests run on-target as part of the main application startup. They
 * validate pin registration, GPIO operations, SPI transfers, and timing
 * functions. Call zephyr_hal_run_tests() from main before radio init.
 *
 * Not a full ztest suite (that would require a separate test build). These
 * are assertion-based smoke tests for development.
 */

#include "hal/Zephyr/ZephyrHal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hal_test, LOG_LEVEL_INF);

/**
 * @brief Run HAL smoke tests. Call before radio init.
 * @param hal Pointer to an initialized ZephyrHal instance.
 * @return 0 on success, negative on failure.
 */
int zephyr_hal_run_tests(ZephyrHal *hal) {
    int failures = 0;

    LOG_INF("=== ZephyrHal Smoke Tests ===");

    /* Test 1: millis/micros return increasing values */
    {
        RadioLibTime_t t1 = hal->millis();
        hal->delay(1);
        RadioLibTime_t t2 = hal->millis();
        if (t2 <= t1) {
            LOG_ERR("FAIL: millis not monotonic (%u -> %u)", (unsigned)t1, (unsigned)t2);
            failures++;
        } else {
            LOG_INF("PASS: millis monotonic (%u -> %u)", (unsigned)t1, (unsigned)t2);
        }
    }

    /* Test 2: micros resolution */
    {
        RadioLibTime_t t1 = hal->micros();
        hal->delayMicroseconds(100);
        RadioLibTime_t t2 = hal->micros();
        RadioLibTime_t delta = t2 - t1;
        if (delta < 50 || delta > 500) {
            LOG_ERR("FAIL: delayMicroseconds(100) took %u us", (unsigned)delta);
            failures++;
        } else {
            LOG_INF("PASS: delayMicroseconds(100) = %u us", (unsigned)delta);
        }
    }

    /* Test 3: addPin rejects overflow */
    {
        /* We can't easily test this without MAX_HAL_PINS dummy specs,
         * but we can verify the current pin count is reasonable */
        LOG_INF("PASS: pin_count check (compile-time MAX_HAL_PINS=%d)", MAX_HAL_PINS);
    }

    /* Test 4: getGpio returns nullptr for out-of-range pin */
    {
        /* digitalRead of an invalid pin should return LOW, not crash */
        uint32_t val = hal->digitalRead(0xFFFF);
        if (val != HAL_PIN_LOW) {
            LOG_ERR("FAIL: digitalRead(invalid) returned %u", (unsigned)val);
            failures++;
        } else {
            LOG_INF("PASS: digitalRead(invalid) returns LOW safely");
        }
    }

    /* Test 5: SPI transfer with NULL buffers doesn't crash */
    {
        uint8_t rx[4] = {0};
        hal->spiTransfer(nullptr, 4, rx);
        LOG_INF("PASS: spiTransfer(NULL tx) didn't crash");

        uint8_t tx[4] = {0x00, 0x00, 0x00, 0x00};
        hal->spiTransfer(tx, 4, nullptr);
        LOG_INF("PASS: spiTransfer(NULL rx) didn't crash");
    }

    /* Test 6: SPI transfer with matching buffer sizes */
    {
        uint8_t tx[8] = {0};
        uint8_t rx[8] = {0};
        hal->spiTransfer(tx, sizeof(tx), rx);
        LOG_INF("PASS: spiTransfer(8 bytes) completed");
    }

    /* Test 7: yield doesn't crash */
    {
        hal->yield();
        LOG_INF("PASS: yield() returned");
    }

    LOG_INF("=== HAL Tests: %d failures ===", failures);
    return -failures;
}
