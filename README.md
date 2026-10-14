# ESPNES

A NES emulator for ESP32 boards with FabGL's VGA and PS/2 wiring, built for an ESP32-PICO-D4
board: 4 MB of flash on the chip, **no PSRAM, no SD card**. ROMs live in the SPIFFS partition.

## Status

- **CPU:** 2A03, all official and the stable unofficial opcodes, exact cycle counts. Passes
  nestest's automated mode cycle for cycle (`make -C host test`).
- **PPU:** scanline renderer with the loopy scroll model, so `$2005`/`$2006` splits work; sprite 0
  hit is timed to the dot, 8 sprites per line, 8x16 sprites, left column clipping, greyscale.
  NTSC (262 lines, 3 dots per CPU cycle) and PAL (312 lines, 3.2 dots) timing.
- **Mappers:** 0 (NROM). The cartridge layer works through bank pointer tables, ready for MMCs.
- **Not yet:** sound (APU registers are stored, not played), colour emphasis, PPU open bus decay.

Super Mario Bros. runs at full speed with no frame skipping: ~15 ms of emulation per 16.6 ms frame.

## Video

320x240 at 60 Hz with the 256x240 picture centred, black borders left and right. The PPU writes
raw pixels straight into FabGL's DMA line buffers, so the output costs no interrupt time. The
board has 2 bits per colour channel, so the 64 NES colours are approximated with RGB222. PAL games
are emulated at 50 frames per second on the 60 Hz signal.

## Keys

| NES    | Keyboard          |
|--------|-------------------|
| D-pad  | arrow keys        |
| A      | Ctrl              |
| B      | Alt               |
| Select | Shift             |
| Start  | Space             |

Esc goes back to the ROM list, F1 resets. In the list, Left/Right picks the TV system
(Auto uses the NES 2.0 header, then names like "(E)" or "(Europe)", otherwise NTSC).

Over the serial console (115200 baud) the ROM list is printed at boot; sending a number starts
that ROM and `q` leaves the game. Frame timings are printed every 5 seconds.

## Build & flash

```sh
pio run -t upload       # firmware (~/.platformio/penv/bin/pio if pio is not on the PATH)
pio run -t uploadfs     # everything in roms/ into SPIFFS (2.4 MB)
pio device monitor
```

SPIFFS has no directories and names are limited to 31 characters, so keep ROM file names short.

## Layout

- `src/nes/` the emulator core, plain C++ without ESP32 dependencies
- `src/main.cpp` the ESP32 side: VGA, keyboard, ROM list, frame pacing
- `host/` PC build of the core: nestest check, frame dumps to PPM, a benchmark
- `lib/FabGL/` FabGL 1.0.9, vendored (see `lib/FabGL/VENDORED.md`)
- `roms/` packed into SPIFFS by `uploadfs`
- `testroms/` test ROMs for the host harness (nestest)
