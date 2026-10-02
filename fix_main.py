import re

with open('src/main.cpp', 'r') as f:
    text = f.read()

# 1. USB init and enable/disable
text = text.replace('setup_usb_storage();', 'setup_usb_storage();\n    tinygs_usb_init();')
text = text.replace('usb_enable(NULL)', 'tinygs_usb_enable()')
text = text.replace('usb_disable()', 'tinygs_usb_disable()')
text = text.replace('cdc_acm_dte_rate_callback_set(console_dev, baudrate_reset_handler);', '')

# 2. Expose USB functions and baudrate handler
text = text.replace('static void baudrate_reset_handler(const struct device *dev, uint32_t baudrate)', 
                    'void baudrate_reset_handler(const struct device *dev, uint32_t baudrate)')
text = 'int tinygs_usb_init(void);\nint tinygs_usb_enable(void);\nint tinygs_usb_disable(void);\n\n' + text

# 3. OpenThread Locking
text = text.replace('openthread_api_mutex_lock(ctx);', 'openthread_mutex_lock();')
text = text.replace('openthread_api_mutex_unlock(ctx);', 'openthread_mutex_unlock();')
text = text.replace('openthread_api_mutex_lock(ot_ctx);', 'openthread_mutex_lock();')
text = text.replace('openthread_api_mutex_unlock(ot_ctx);', 'openthread_mutex_unlock();')

# 4. OpenThread state changed callback
text = text.replace('static void ot_state_changed_handler(otChangedFlags flags,\n                                     struct openthread_context *ot_context,\n                                     void *user_data)',
                    'static void ot_state_changed_handler(otChangedFlags flags, void *aContext)')

text = text.replace('static struct openthread_state_changed_cb ot_state_cb = {\n    .state_changed_cb = ot_state_changed_handler,\n};',
                    'static struct openthread_state_changed_callback ot_state_cb = {\n    .otCallback = ot_state_changed_handler,\n};')

text = text.replace('openthread_state_changed_cb_register(ctx, &ot_state_cb);',
                    'openthread_state_changed_callback_register(&ot_state_cb);')

# 5. Fix unused `ctx` where they were only used for locking.
# We'll just leave `ctx` there and add `(void)ctx;` to suppress unused variable warnings.
# Or better, we just let `#pragma GCC diagnostic ignored "-Wunused-variable"` handle it.
# Actually I'll add `#pragma GCC diagnostic ignored "-Wunused-variable"` to the top of the file just in case.
text = '#pragma GCC diagnostic ignored "-Wunused-variable"\n' + text

# 6. Fix SPI_DT_SPEC_GET deprecation
text = text.replace('SPI_DT_SPEC_GET(DT_NODELABEL(sx1262), SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0)',
                    'SPI_DT_SPEC_GET(DT_NODELABEL(sx1262), SPI_WORD_SET(8) | SPI_TRANSFER_MSB)')

with open('src/main.cpp', 'w') as f:
    f.write(text)

