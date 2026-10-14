# ESPNES

A NES emulator for ESP32 boards with FabGL's VGA and PS/2 wiring, built for an ESP32-PICO-D4
board: 4 MB of flash on the chip, **no PSRAM, no SD card**. The ROMs are built into the firmware.

## Status

- **CPU:** 2A03, all official and the stable unofficial opcodes, exact cycle counts. Passes
  nestest's automated mode cycle for cycle (`make -C host test`).
- **PPU:** scanline renderer with the loopy scroll model, so `$2005`/`$2006` splits work; sprite 0
  hit is timed to the dot, 8 sprites per line, 8x16 sprites, left column clipping, greyscale.
  NTSC (262 lines, 3 dots per CPU cycle) and PAL (312 lines, 3.2 dots) timing.
- **Mappers:** 0 (NROM), 1 (MMC1, including SUROM's 512 KB), 2 (UxROM), 4 (MMC3, with its scanline IRQ). The cartridge layer works through bank pointer tables, so a
  mapper only repoints 8 KB PRG and 1 KB CHR pages.
- **APU:** both pulse channels with sweep and envelope, triangle, noise, DMC sample playback with
  its IRQ, the frame counter (4 and 5 step, frame IRQ), `$4015` status. Output at 22050 Hz on the
  DAC (GPIO 25); each sample averages the channels over its time span, so high notes do not alias
  much. DMC DMA cycle stealing is not emulated.
- **Not yet:** colour emphasis, PPU open bus decay.

Super Mario Bros. runs at full speed with no frame skipping, using ~8.8 ms of the 16.6 ms frame.

## Sound

The APU runs on core 1 with the CPU, lazily: it catches up whenever an APU register is touched and
once per scanline. Its samples go through a ring buffer to FabGL's SoundGenerator, which feeds the
DAC from an I2S interrupt. The emulation is paced by a timer and the DAC by the I2S clock, so the
APU's sample rate is nudged by up to 0.5% to keep about three frames buffered. F2/F3 set the volume.

## Cores

Core 1 runs the CPU and the PPU's timing; core 0 draws the picture. At the start of each visible
line core 1 snapshots the PPU state the line depends on (scroll, control, mask, palette, bank
pointers) and moves on; core 0 draws from those snapshots straight into the VGA frame buffer.
Sprite 0 hit is still decided on core 1 (it only needs the 8 pixels under sprite 0), so the CPU
sees it at the right dot. Before vblank, when games rewrite VRAM and OAM, core 1 waits for core 0
to finish the frame; in practice it never has to.

The drawing code lives in flash on purpose, not IRAM: both cores fetching instructions from IRAM
at once slowed each other down measurably, while flash code runs from each core's own cache.

The serial console prints per frame timings every 5 seconds: emulation time on core 1, drawing
time on core 0, and how long core 1 waited for it.

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

Esc goes back to the ROM list, F1 resets, F2/F3 turn the volume down/up. In the list, Left/Right picks the TV system
(Auto uses the NES 2.0 header, then names like "(E)" or "(Europe)", otherwise NTSC).

Over the serial console (115200 baud) the ROM list is printed at boot; sending a number starts
that ROM, `m` and a number shows its manual (`n`/`p` turn pages, `x` leaves), `q` leaves the game; `a b e s u d l r` tap A, B, Select, Start and the directions. Frame timings are printed every 5 seconds.

## ROMs

Every `.nes` file in `roms/` is built into the firmware: `tools/embed_roms.py` runs before each
build and generates `src/generated/` (an `.incbin` per ROM and the index the ROM list shows).

`roms/settings.csv` sets the menu: one game per line, in menu order, `#` lines are comments:

```
"Legend of Zelda, The (Europe).nes","The Legend of Zelda","zelda1.png","zelda1.txt"
```

File name, name shown in the menu, picture, manual, options (not used yet). A ROM
the CSV does not list is still built in, at the end under its file name, with a warning; a
malformed line (a missing quote, say) is reported and skipped.

The picture is a PNG in `roms/`, scaled to fit 64x64 (aspect kept, black bars) and dithered to the
NES palette by `tools/nesimage.py` (plain Python, no Pillow needed). Converted pictures are cached in
`src/generated/pics/`. A missing picture is a build warning and an empty square in the menu.
`python3 tools/nesimage.py picture.png preview.ppm` shows what a picture will look like.

The manual is a text file in `roms/` of teletext style pages, 32 columns by 25 lines, opened with M
or Tab in the menu (Left/Right turn the pages, Esc goes back):

```
[PAGESTART=1]
The Legend of Zelda \[PAL\]

[color=red]Story
[color=white]The kingdom of Hyrule has fallen
[PAGEEND]
```

`[color=...]` (black red green yellow blue magenta cyan white) lasts until the next one; every page
starts in white. `\[`, `\]` and `\\` are literal brackets and backslashes. A line that holds only
tags gives no line on the screen. Longer lines and pages are cut with a build warning, nothing is
wrapped. Umlauts work (the font is code page 437). On the device it is stored as a cut-down VT100
form (see `tools/nesmanual.py`); `python3 tools/nesmanual.py roms/zelda1.txt` previews a manual in
the terminal with its colours and warnings.
Constant data in flash is mapped into the address space through the cache, so the emulator reads
a ROM in place. Nothing is copied to RAM, which a 128 KB game would not fit into anyway, and
nothing is written to flash at runtime. Changing the games means rebuilding and flashing.

Reading PRG from flash through the cache costs nothing measurable (SMB: 8.7 ms per frame either
way). The firmware partition is 3.4 MB, so about 2.7 MB of ROMs fit next to the code.

## Save games

Games with a battery (the header's battery flag) keep their 8 KB of save RAM in the 512 KB SPIFFS
partition. It is loaded when the game starts and written back when you return to the ROM list
with Esc, only if it changed. A reset (F1) keeps it, as on the console. Switching the board off
while in a game loses what happened since the game was started, so go back to the list first.

Save files are named after the start of the ROM's name plus the CRC32 of its contents, so
renaming a ROM keeps its save. Some games (Zelda) also use that RAM as working memory, so it
changes every session and gets written on every return to the list.

## Per game settings

An optional `Game.cfg` next to `Game.nes` in `roms/` is built in with it:

```
# yes/no: battery save RAM, overriding the header (some games use the RAM without a battery)
save = no
# ntsc/pal: TV system, overriding the header and the "(Europe)"-style name tags
region = pal
```

The TV choice in the ROM list (Left/Right), when not Auto, overrides both.

## Build & flash

```sh
pio run -t upload       # firmware with the ROMs (~/.platformio/penv/bin/pio if pio is not on the PATH)
pio device monitor
```

The 512 KB SPIFFS partition is formatted on the first boot and holds the save games.

## Layout

- `src/nes/` the emulator core, plain C++ without ESP32 dependencies
- `src/main.cpp` the ESP32 side: VGA, keyboard, ROM list, frame pacing
- `host/` PC build of the core: nestest check, frame dumps to PPM (`async` draws on a second
  thread like the ESP32 build), sound to WAV (`wav=file.wav`), save RAM in and out (`sram=file`), a trace of PPU register writes per scanline and dot
  (`trace=FROM-TO` frames), a benchmark
- `lib/FabGL/` FabGL 1.0.9, vendored (see `lib/FabGL/VENDORED.md`)
- `roms/` built into the firmware by `tools/embed_roms.py`
- `testroms/` test ROMs for the host harness (nestest)
