# v3.3 SPI / GPIO crash — analysis notes

## Symptom

Deterministic boot crash on every v3.3 boot at +1.7s of uptime:
- `PC=0x14470858` (constant; invalid memory)
- `LR` varies but always resolves to `_spi_context_cs_control` at `spi_context.h:387`
- `reason=35` = `K_ERR_ARM_MEM_INSTRUCTION_ACCESS` (instruction fetch fault)
- `crash_thread='main'`
- Boot reaches: USB up → banner → "Starting OpenThread" → "Thread role: Detached" → CRASH

## What we proved

### Not the cause
1. **Display init** — disabling `tinygs_display_init()` doesn't fix it
2. **WS2812 / LED strip** — disabling `CONFIG_LED_STRIP` and `CONFIG_WS2812_STRIP_SPI` doesn't fix it
3. **Stack overflow** — doubling main stack 8K→16K doesn't fix it
4. **LTO** — `CONFIG_LTO=n` (which bumps UF2 from 0.99MB to 1.05MB) doesn't fix it
5. **SETTINGS_NVS pin alone** — fixes a different crash (unrelated to this one)

### Confirmed about the crash itself
- Disasm of LR=0x84fa1: returning from `bl gpio_pin_set_dt.isra.0` inside
  `_spi_context_cs_control`, then loads `r3 = ctx->config; r0 = ctx->config->cs.delay;`
  before tail-calling `b.w z_impl_k_busy_wait`.
- The bad PC=0x14470858 is reached via `gpio_pin_set_dt` → `gpio_pin_set_raw` →
  `port->api->port_set_bits_raw(port, BIT(pin))`. The vtable jump goes to
  garbage, meaning either:
  - `port` itself is corrupted (most likely)
  - `port->api` table is corrupted
  - The `port_set_bits_raw` function pointer in `gpio_nrfx_drv_api_funcs`
    is corrupted
- `gpio_nrfx_drv_api_funcs` at 0x99338 was dumped — all entries look valid
  (in flash range, thumb bit set). So **the GPIO API table is fine**.
- That means `ctx->config->cs.gpio.port` is corrupted on the call site.

### What ISN'T explained
- Where the corrupted `port` comes from. The MIPI DBI driver's
  `MIPI_DBI_SPI_CONFIG_DT` macro builds `cs.gpio` from
  `MIPI_DBI_SPI_CS_GPIOS_DT_SPEC_GET` which uses
  `GPIO_DT_SPEC_GET_BY_IDX_OR(spi-dev, cs_gpios, REG_ADDR, {})`.
  For `st7789v@0` (reg=<0>) under `mipi_dbi_st7789v` (spi-dev=&spi0),
  this looks up index 0 of spi0's cs-gpios = `<&gpio0 11 GPIO_ACTIVE_LOW>`.
  That should resolve to a valid `gpio_dt_spec`.

### What we haven't tried
- Disable spi0 entirely (`status = "disabled"`) — would prove whether
  display SPI init is the trigger
- Disable spi1 entirely — would prove whether LoRa SPI is the trigger
- Disable BOTH and see if the crash moves elsewhere — would prove SPI is
  the family
- Remove cs-gpios from spi0 entirely (since with the MIPI DBI wrapper,
  spi0 has no direct child anyway) — minimal-risk experiment
- Build a known-good v3.3 sample (e.g., `samples/drivers/led/led_strip`)
  for the same board to verify the toolchain produces working binaries
- Step-through with JTAG to read actual register state at fault

## Most-likely remaining hypotheses (in order of probability)

1. **spi0 cs-gpios ↔ MIPI DBI binding mismatch** — In v3.3 the DT
   semantics may require the device using CS to be a direct child of
   the SPI bus. Our `st7789v@0` is a child of `mipi_dbi_st7789v` (the
   wrapper), not of `&spi0`. If the SPI driver's `cs_gpios` array and
   indices assume direct children, our setup may build a corrupted CS
   spec that points outside the array.

2. **Init order** — Some consumer of the SPI peripheral runs at
   `POST_KERNEL` priority lower than 50 (SPI init). If it tries to
   transfer before SPI is ready, `ctx->config` is uninitialised /
   garbage.

3. **Toolchain bug** — possible but unlikely; our binary builds clean,
   nothing else is observably wrong.

4. **LTO clone artifacts** — ruled out by testing with LTO=n.

## Recommended next steps

1. Apply DT bisect — disable each SPI controller in turn:
   - `&spi0 { status = "disabled"; };` (display)
   - `&spi1 { status = "disabled"; };` (LoRa)
   - `&spi2 { status = "disabled"; };` (NeoPixel)

2. If isolating to spi0: try moving the `mipi_dbi_st7789v` wrapper to
   be a child of `&spi0` (instead of root). Check NCS sample for
   exact pattern. The reference
   `boards/shields/st7789v_generic/st7789v_waveshare_240x240.overlay`
   has the wrapper at root, but maybe the example doesn't actually exercise
   the CS path the same way ours does.

3. Add an `early_log` line at the top of `main()` and at every step
   to pin down the crash location more precisely (right now we know
   it's between "Thread role: Detached" log and the next reboot, but
   that window includes several main-thread function calls).

4. As a last resort: bring up a known-working v3.3 nrf52840 SPI
   sample (e.g., `samples/drivers/led/led_strip`) on the T114 to verify
   the toolchain produces a working binary at all.
