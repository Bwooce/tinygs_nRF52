# v3.3.0-specific changes to apply at switch-over

These changes break the v2.6 build and must only be applied once we're ready
to fully switch to ncs_new/ + SDK 0.17.4.

## 1. prj.conf

Add explicit pin to keep NVS as the settings backend (v3.0 default became ZMS):

```diff
 # CONFIG_SETTINGS is implicitly enabled by CONFIG_OPENTHREAD_PLATFORM_KEY_REF,
 # which OpenThread pulls in for dataset / network-key persistence. Re-stating
 # it here used to cause double-registration of the "ot" subtree handler and
 # boot-time asserts — leave it inferred. tinygs_config.cpp registers the
 # "tgs" subtree alongside OpenThread's "ot" subtree on the shared 8 KB
-# storage_partition (see app.overlay). CONFIG_SETTINGS_RUNTIME /
-# CONFIG_SETTINGS_NVS are likewise auto-enabled — do not add them here.
+# storage_partition (see app.overlay). On v3.x the default settings backend
+# became ZMS — explicitly pin NVS to avoid silent on-flash format migration
+# that would lose saved Thread credentials and station config.
+CONFIG_SETTINGS_NVS=y
```

## 2. app.overlay — ST7789V → MIPI DBI

Reference pattern: `ncs_new/zephyr/boards/shields/st7789v_generic/st7789v_waveshare_240x240.overlay`

```diff
 &spi0 {
     compatible = "nordic,nrf-spim";
     status = "okay";
     pinctrl-0 = <&spi0_default>;
     pinctrl-1 = <&spi0_sleep>;
     pinctrl-names = "default", "sleep";
-    cs-gpios = <&gpio0 11 GPIO_ACTIVE_LOW>;
-
-    st7789v: st7789v@0 {
-        compatible = "sitronix,st7789v";
-        reg = <0>;
-        spi-max-frequency = <8000000>;
-        cmd-data-gpios = <&gpio0 12 GPIO_ACTIVE_LOW>;
-        reset-gpios = <&gpio0 2 GPIO_ACTIVE_LOW>;
-        width = <240>;
-        ...
-    };
+    cs-gpios = <&gpio0 11 GPIO_ACTIVE_LOW>;
+};
+
+/ {
+    mipi_dbi_st7789v {
+        compatible = "zephyr,mipi-dbi-spi";
+        spi-dev = <&spi0>;
+        dc-gpios = <&gpio0 12 GPIO_ACTIVE_LOW>;
+        reset-gpios = <&gpio0 2 GPIO_ACTIVE_LOW>;
+        write-only;
+        #address-cells = <1>;
+        #size-cells = <0>;
+
+        st7789v: st7789v@0 {
+            compatible = "sitronix,st7789v";
+            reg = <0>;
+            mipi-max-frequency = <8000000>;
+            mipi-mode = "MIPI_DBI_MODE_SPI_4WIRE";
+            width = <240>;
+            height = <135>;
+            x-offset = <40>;
+            y-offset = <53>;
+            vcom = <0x19>;
+            gctrl = <0x35>;
+            vrhs = <0x12>;
+            vdvs = <0x20>;
+            mdac = <0x60>;
+            gamma = <0x01>;
+            colmod = <0x55>;
+            lcm = <0x2c>;
+            porch-param = [0c 0c 00 33 33];
+            cmd2en-param = [5a 69 02 01];
+            pwctrl1-param = [a4 a1];
+            pvgam-param = [D0 04 0D 11 13 2B 3F 54 4C 18 0D 0B 1F 23];
+            nvgam-param = [D0 04 0C 11 13 2C 3F 44 51 2F 1F 1F 20 23];
+            ram-param = [00 F0];
+            rgb-param = [CD 08 14];
+        };
+    };
+};
```

Key per-property changes:
- `spi-max-frequency` → `mipi-max-frequency` (on the inner sitronix node)
- `cmd-data-gpios` → `dc-gpios` (moves to wrapper, renamed)
- `reset-gpios` → `reset-gpios` (moves to wrapper, same name)
- New: `mipi-mode = "MIPI_DBI_MODE_SPI_4WIRE"`
- New: `write-only` flag on wrapper (display is write-only)

Display init code in `tinygs_display.cpp` should be unchanged — it uses
`DEVICE_DT_GET_OR_NULL(DT_NODELABEL(st7789v))` which still resolves to the
same node, just nested inside the MIPI DBI wrapper now.

## 3. build.sh — point at new workspace

```diff
-export ZEPHYR_BASE=/home/bruce/dev/tinygs_nRF52/ncs/zephyr
+export ZEPHYR_BASE=/home/bruce/dev/tinygs_nRF52/ncs_new/zephyr
-export ZEPHYR_SDK_INSTALL_DIR=/home/bruce/zephyr-sdk-0.16.8
+export ZEPHYR_SDK_INSTALL_DIR=/home/bruce/zephyr-sdk-0.17.4
```

## 4. Anticipated kconfig surface drift

Run a build and walk all warnings. Likely candidates from the migration guide:
- USB device classic stack: `CONFIG_USB_DEVICE_STACK=y` still works (legacy
  stack still supported in v3.3).
- mbedTLS: `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` still honoured per Q1 research.
- mbedTLS legacy crypto: may need explicit `CONFIG_MBEDTLS_LEGACY_CRYPTO_C=y`.
- Anything renamed will produce a build warning; act on each one.
