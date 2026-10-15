# Vendored FabGL

This is **FabGL 1.0.9** by Fabrizio Di Vittorio, copied from `../FabGL-master`
(`src/`, `library.json`, `library.properties`, `LICENSE`). GPL v3, see `LICENSE`.

- Upstream: https://github.com/fdivitto/FabGL (v1.0.9)
- Vendored on: 2026-10-14

Vendored rather than a `lib_deps` entry so local changes survive `pio run -t fullclean` and the
project builds offline, the same reasoning as in ESPPC.

## Local modifications

Record every change here as it is made:

| File | Change | Why |
|------|--------|-----|
| `library.json` | `build.srcFilter`: only the files ESPNES uses are compiled | the rest (GUI, terminal, emulated devices, composite video, other drivers) does not build with ESP-IDF 5 and is not needed |
| `src/fabutils.h` | with IDF >= 5: includes `esp_timer.h`, `soc/io_mux_reg.h`, `soc/rtc_io_reg.h`, `soc/gpio_sig_map.h`, `soc/gpio_periph.h`, `esp32/rom/gpio.h`, `driver/gpio.h` | IDF 5 no longer pulls them in through other headers |
| `src/devdrivers/swgenerator.cpp` | with IDF >= 5: `rtc_clk_apll_enable(bool)` plus `rtc_clk_apll_coeff_set()` | the APLL API changed in IDF 5 (the VGA dot clock) |
| `src/fabutils.cpp` | with IDF >= 5: no `esp_task_wdt_init(45, false)`; `f_mkfs()` with `MKFS_PARM` | both APIs changed in IDF 5 (only used to format SD cards) |
| `src/dispdrivers/vgabasecontroller.cpp` | `getDMABuffer()` casts away `const` | IDF 5's `lldesc_t::buf` is `const volatile` |
| `src/fabui_stub.cpp` (new) | `uiApp::postEvent()` returning false | the keyboard and mouse drivers reference it; ESPNES has no FabGL GUI, and `fabui.cpp` does not build with IDF 5 |
