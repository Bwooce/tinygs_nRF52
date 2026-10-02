#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(tinygs_usb, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(tinygs_usbd,
           DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
           0x2FE3, 0x0001);

USBD_DESC_LANG_DEFINE(tinygs_usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(tinygs_usb_mfr, "TinyGS");
USBD_DESC_PRODUCT_DEFINE(tinygs_usb_product, "TinyGS Configurator");

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(tinygs_fs_config,
              USB_SCD_SELF_POWERED,
              100, &fs_cfg_desc);

USBD_DEFINE_MSC_LUN(nand, "NAND", "Zephyr", "FlashDisk", "0.00");

extern void baudrate_reset_handler(const struct device *dev, uint32_t baudrate);

static void usbd_msg_cb(struct usbd_context *ctx, const struct usbd_msg *msg)
{
    if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
        uint32_t baudrate = 0;
        uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_BAUD_RATE, &baudrate);
        baudrate_reset_handler(msg->dev, baudrate);
    }
}

int tinygs_usb_init(void)
{
    int err;

    err = usbd_add_descriptor(&tinygs_usbd, &tinygs_usb_lang);
    err |= usbd_add_descriptor(&tinygs_usbd, &tinygs_usb_mfr);
    err |= usbd_add_descriptor(&tinygs_usbd, &tinygs_usb_product);
    if (err) {
        LOG_ERR("Failed to init descriptors");
        return err;
    }

    err = usbd_add_configuration(&tinygs_usbd, USBD_SPEED_FS, &tinygs_fs_config);
    if (err) {
        LOG_ERR("Failed to add FS config");
        return err;
    }

    err = usbd_register_class(&tinygs_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
    if (err && err != -EALREADY) LOG_ERR("Failed to register cdc_acm_0");

    err = usbd_register_class(&tinygs_usbd, "msc_0", USBD_SPEED_FS, 1);
    if (err && err != -EALREADY) LOG_ERR("Failed to register msc_0");

    err = usbd_device_set_code_triple(&tinygs_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    
    err = usbd_msg_register_cb(&tinygs_usbd, usbd_msg_cb);

    err = usbd_init(&tinygs_usbd);
    if (err) {
        LOG_ERR("Failed to init USBD");
        return err;
    }

    return 0;
}

int tinygs_usb_enable(void)
{
    return usbd_enable(&tinygs_usbd);
}

int tinygs_usb_disable(void)
{
    return usbd_disable(&tinygs_usbd);
}
