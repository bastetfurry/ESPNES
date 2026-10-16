# ESPNES

A NES emulator for ESP32 boards with FabGL's VGA and PS/2 wiring, built for an ESP32-PICO-D4
board: 4 MB of flash on the chip, **no PSRAM, no SD card**. The ROMs are built into the firmware.

## Status

- **CPU:** 2A03, all official and the stable unofficial opcodes, exact cycle counts. Passes
  nestest's automated mode cycle for cycle (`make -C host test`).
- **PPU:** scanline renderer with the loopy scroll model, so `$2005`/`$2006` splits work; sprite 0
  hit is timed to the dot, 8 sprites per line, 8x16 sprites, left column clipping, greyscale.
  Sprite evaluation follows the hardware byte by byte: it starts at OAMADDR (which rendering
  resets to 0 every line), re-aligns a misaligned address, and has the overflow flag bug.
  NTSC (262 lines, 3 dots per CPU cycle) and PAL (312 lines, 3.2 dots) timing.
- **Mappers:** 0 (NROM), 1 (MMC1, including SUROM's 512 KB), 2 (UxROM), 4 (MMC3, with its scanline
  IRQ), 5 (MMC5: all PRG/CHR modes, separate sprite/background CHR sets for 8x16 sprites, ExRAM
  as nametable, fill mode, scanline IRQ, multiplier; not its extended attributes, split screen or
  sound), 7 (AxROM, with AOROM bus conflicts), 71 (Camerica/Codemasters, with Fire Hawk's
  mirroring). The cartridge layer works through bank pointer tables, so a
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

| NES    | Player 1   | Player 2 (numpad) |
|--------|------------|-------------------|
| D-pad  | arrow keys | 8 4 5 6           |
| A      | Ctrl       | .                 |
| B      | Alt        | 0                 |
| Select | Shift      | /                 |
| Start  | Space      | *                 |
| Mic    |            | +                 |

These are the defaults. K in the ROM list opens the key mapper: pick the player (1 or 2), then a
drawn pad shows the button being set blinking; press a key for each button in turn (Up, Down,
Left, Right, Select, Start, B, A, and for player 2 the Mic). A key another button (of either
player) already has swaps with it, Esc cancels. The keys are the same for all games and kept in
SPIFFS (`/keys.cfg`), written only when they change. Left and right Ctrl/Alt/Shift count as one
key, a letter works with Shift held, and a numpad key is the same with or without Num Lock (but
not the same as the arrow keys). Esc, F1-F3, F5-F7, F10, Scroll Lock and SysRq cannot be used.

Mic is the Famicom's microphone on the second pad: while it is held, `$4016` bit 2 reads noise,
like someone talking into it (Famicom games use it, e.g. the Japanese Zelda's Pols Voice).

### System menu

Alt+PrtScr (SysRq) opens the system menu over the paused game, the same key as in ESPPC. It has
the tape (play, record, stop, and what is on it), the keyboard (Famicom or pads, for keyboard
games), cheats on/off, the volume (Left/Right), reset and back to the ROM list. Esc or SysRq
returns to the game.

### Family BASIC keyboard

Games with "BASIC" in the file name, or `keyboard=yes` in their options, get the Family BASIC
keyboard: the PC keyboard becomes the Famicom's (`$4016` writes select the row and column,
`$4017` bits 1-4 read the keys, see `src/nes/famikey.h`). Keys sit where they are on the
Famicom, judged by FabGL's US layout, so Z and Y are swapped on a German keyboard. Only SysRq and
Scroll Lock stay the emulator's; Scroll Lock switches the keyboard to the pads and back.

| Famicom | PC | Famicom | PC |
|---|---|---|---|
| ESC | Esc, Tab | STOP | Pause/Break |
| CTR | Ctrl | GRPH | Alt |
| KANA | AltGr | CLR HOME | Home |
| INS | Insert | DEL | Backspace, Delete |
| @ [ ] | [ ] \\ | ¥ | \` |
| ^ | = | : | ' |
| _ | Menu key | F1-F8 | F1-F8 |

Use Family BASIC V3.0, which has 4 KB for programs (4086 bytes free). V2.x has 2 KB. SAVE and LOAD
go to the Data Recorder: press record (or play) in the system menu, then type SAVE "NAME" (or
LOAD). `host/nestool run ... type=F:TEXT` types on the keyboard from frame F (`\n` is RETURN).

### Real pads

NES pads can be wired to free GPIOs: latch and clock shared by both pads, a data line each. Set
the pins at `PAD_LATCH_GPIO`, `PAD_CLOCK_GPIO` and `PAD_DATA_GPIO` in `src/main.cpp` (-1 = not
connected). Real pads and keys work at the same time.

- Power the pads from **3.3 V**, not 5 V: the pad's 4021 runs on it, and its data line then
  cannot put 5 V on the ESP32.
- The data lines need a pull-up, so an unplugged pad reads as no buttons: the firmware turns on
  the internal one. GPIO 34-39 have none and need an external one (10 kΩ to 3.3 V).
- Taken: VGA 4, 5, 15, 18, 19, 21, 22, 23; PS/2 keyboard 32, 33; sound 25. Avoid the strapping
  pins 0, 2 and 12. Left as candidates (on a LilyGO/TTGO VGA32): 13 and 14 (the SD slot's on
  boards that have one; the older board ESPNES runs on has none), 26 and 27 only if the second
  PS/2 socket (mouse) stays empty, and 34-39 for data lines only (inputs, external pull-up).
- NES port, pin 1 GND, 2 clock, 3 latch, 4 data, 7 +V (pins 5 and 6 are the Zapper's); check
  with a meter, clones and extension cables differ.

## Savestates

"Back to the ROM list" in the system menu (SysRq) pauses the game and shows the ROM list, with a green arrow at the paused game. Enter on it
carries on where it was; starting or loading another game ends it. Nothing is copied for that: the
console just waits.

S in the list saves the paused game's state in slot 1 or 2, L loads the selected game from one.
Each game has two slots; under the picture the list shows which hold a state. A state is 1-12 KB
(SMB 1.5 KB, Zelda 12-14 KB): the console's RAM, nametables, sprites and palette, the cartridge's
RAM and CHR RAM, and the registers, compressed with a small LZ that needs no buffer
(`src/nes/state.h`). They are files in SPIFFS next to the saves (`.st1`, `.st2`); a save is
refused when it would leave less than 48 KB of SPIFFS free, so the file system keeps room to
work. A new state is written to a new file first, so a failed save keeps the old one.

A state includes the battery backed RAM: after loading one, leaving the game writes that into the
game's save file, as an emulator does. States belong to this version of the emulator; one made by
a version with a different state format is refused (`STATE_VERSION` in `src/nes/nes.cpp`).

F in the list opens the file manager: everything in SPIFFS by game (saves, codes, states, the
keys) with its size, and how full SPIFFS is. Files of games that are no longer built in are red:
their names carry the CRC of a ROM no game in the list has. Del deletes the selected file, O all
files of removed games; both ask first.

`host/nestool run ... statetest=F` saves a state at frame F, runs to the end, loads it and runs the
same frames again: picture, RAM and CPU must come out the same (all games here do).

SysRq (Alt+PrtScr) opens the system menu, in every game (see below). Esc belongs to the game; in menus and dialogs it goes back. F1 resets, F2/F3 turn the volume down/up, F5/F6/F7 play, record and stop the tape, and F10 switches cheats. In the list, Left/Right picks the TV system
(Auto uses the NES 2.0 header, then names like "(E)" or "(Europe)", otherwise NTSC).

Over the serial console (115200 baud) the ROM list is printed at boot; sending a number starts
that ROM, `m` and a number shows its manual (`n`/`p` turn pages, `x` leaves), `s` and a slot saves the paused game's state, `l`, a slot and a game number (`l1 0`) loads one, `f` opens the file manager (`n`/`p` select, `d` deletes, `o` deletes the files of removed games, `y` confirms, `~` leaves), `k` opens the key mapper (`1`/`2` picks the player, then a letter, digit or space sets the next button, `^` takes the player's defaults, `~` cancels), `q` leaves the game, `y` opens the system menu (`n`/`p` select, `o` does it, `<`/`>` volume, `~` closes), `T` and a line types it on the Famicom keyboard (`|` is RETURN); `a b e s u d l r` tap A, B, Select, Start and the directions; `P`, `R`, `X` play, record and stop the tape, `U` puts one in (see Data Recorder); while a tape trap asks, `1`-`4`, `p` and `~` answer it. Frame timings are printed every 5 seconds.

## ROMs

Every `.nes` file in `roms/` is built into the firmware: `tools/embed_roms.py` runs before each
build and generates `src/generated/` (an `.incbin` per ROM and the index the ROM list shows).

`roms/settings.csv` sets the menu: one game per line, in menu order, `#` lines are comments:

```
"Legend of Zelda, The (Europe).nes","The Legend of Zelda","zelda1.png","zelda1.txt"
```

File name, name shown in the menu, picture, manual, options (see "Per game settings"). A ROM
the CSV does not list is still built in, at the end under its file name, with a warning; a
malformed line (a missing quote, say) is reported and skipped.

The picture is a PNG in `roms/`, scaled to fit 64x64 (aspect kept, black bars) in NES palette
colours by `tools/nesimage.py` (plain Python, no Pillow needed). Screenshots and other pixel art
(up to 1024 colours) keep their own colours: each 64x64 pixel takes the colour of its source block
closest to the block's average, without dithering, so flat areas stay flat and edges crisp.
Photos and box art are averaged and Floyd-Steinberg dithered. Text smaller than about 16 pixels
does not survive the scaling; a picture cropped to the logo works better than a full title
screen. Converted pictures are cached in
`src/generated/pics/`. In the firmware they are raw deflate compressed (about 300-700 bytes
instead of 4 KB) and unpacked with the inflate in the ESP32's mask ROM, so the decoder costs no
flash. A missing picture is a build warning and an empty square in the menu.
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

## ROMs to try

No commercial ROMs are included. Two that can be had legally:

- **Elite**: Ian Bell, one of its two authors, offers the never-published US (NTSC) version as a
  free download on [his NES Elite page](http://www.iancgbell.clara.net/elite/nes/index.htm)
  (plain HTTP only). MMC1 with battery save, so it also exercises CHR RAM drawing and saves. The
  European cartridge published by Imagineer is a different matter and not covered by that release.
- **nestest** by kevtris, in `testroms/nestest/`: a CPU test with a menu, used by `make -C host test`.
- **AccuracyCoin** by 100th_Coin (MIT, [github.com/100thCoin/AccuracyCoin](https://github.com/100thCoin/AccuracyCoin)),
  in `testroms/accuracycoin/`: 146 tests down to single PPU dots and bus cycles. Start on the page
  index runs them all and shows a table. ESPNES passes 103 or 104 of 146 (2026-10-14), nothing
  skipped: which one depends on the frame Start is pressed in, since that sets the phase of the
  free running DMC against the tests, and the DMC DMA's cycle stealing is only approximated
  (pages 12-16 change, e.g. "DMA + open bus" and "DMA + $2007 read"). The PC harness and the
  board give the same results for the same frame.
  The CPU and opcode pages pass; the failures are the dot exact PPU, DMA and APU timing a scanline
  renderer does not model. (Test 1 of "Misaligned OAM behavior" must pass: its failure path pops
  the test's own return address, which derails the whole run on any emulator that fails it.) For comparison, [doclic's leaderboard](https://doclic.eu/neslead.htm)
  lists Nintendo's NSO emulator at 97 of 140 on an older version of the suite.

## Data Recorder

The Famicom Data Recorder was a cassette recorder plugged into the Family BASIC keyboard. The
console has no tape logic: a game toggles `$4016` bit 0 itself to write and watches `$4016`
bit 1 to read (while bit 2 is set), each game with its own encoding. ESPNES records the signal's
level changes in CPU cycles and plays them back exactly, so every game works the same way. A tape
image (`.ftap`, see `src/nes/tape.h`) stores runs of square-wave periods; games time their bits
with counted loops, so a few periods repeat exactly. Mach Rider's 22 second course save takes
1.5 KB.

On the board, F6 records, F7 stops and F5 plays. The tape is held in RAM, up to 32 KB, and stays
in across games like a cassette left in the recorder. The PC keeps the tapes. When a recording
stops, the board sends it over serial, and `tools/tape.py` saves it and puts tapes back in:

    python3 tools/tape.py record course.ftap     records; Enter stops and saves
    python3 tools/tape.py take course.ftap       saves the next tape recorded with F6/F7
    python3 tools/tape.py play course.ftap       puts it in and plays it
    python3 tools/tape.py put course.ftap        only puts it in (F5 plays)
    python3 tools/tape.py info course.ftap       length, periods, frequencies
    python3 tools/tape.py wav course.ftap a.wav  as sound

Like on the real recorder, press play or record first and then choose LOAD or SAVE in the game.
A recording keeps going until it is stopped, and that includes the pad reads in every frame.

Mach Rider (every version except VS) has SAVE and LOAD for its designed courses in the menu, even
on the NES. There, LOAD waits forever for a tape edge, because `$4016` bit 1 is open bus and
always reads 0. Saving sends about 20 seconds of pulses down the pad strobe line and then
returns.

### Tape traps

Some games skip the tape and save straight from memory. This is what the Wii Virtual Console did
for Mach Rider: its versions call a stub instead of the tape routines. The stub writes `$96` to
`$6000` and `$C3` to `$6001`, then waits until the two are equal while the emulator saves or
loads (`Nes::vcCall`). The call's return address tells a save from a load. For the cart versions,
`tools/tapetrap.py` patches the same stub over the tape loader's entry and points the save's call
at it. Both kinds are recognized from their code at build time, so translations and hacks work
too. `tape=machrider` in the options insists on a trap, with a warning if the routines are not
found, and `tape=none` turns it off to use the real tape.

When the game saves or loads, a line over the picture asks where: 1-4 picks a slot in SPIFFS
(`.tp1` to `.tp4`, listed in the file manager), P picks the PC, and Esc cancels. Over serial, the
same choices are `1`-`4`, `p` and `~`. For the PC, `python3 tools/tape.py file course.bin` takes
the game's next save or answers its next load. The data is Mach Rider's 320 bytes (`$0600`-`$063F`
and `$0700`-`$07FF`), at the same addresses in every version, so a course moves between them.

`python3 tools/tapetrap.py ROM` shows what is found and prints the host harness's
`trap=...` option, which goes with `trapfile=FILE`.

`host/nestool run ... taperec=FILE@F` records from frame F, and `tapeplay=FILE@F` plays a tape.

## Save games

Games with a battery (the header's battery flag) keep their 8 KB of save RAM in the 512 KB SPIFFS
partition. It is loaded when the game starts and written back when you return to the ROM list
only if it changed. A reset (F1) keeps it, as on the console. Switching the board off
while in a game loses what happened since the game was started, so go back to the list first.

Save files are named after the start of the ROM's name plus the CRC32 of its contents, so
renaming a ROM keeps its save. Some games (Zelda) also use that RAM as working memory, so it
changes every session and gets written on every return to the list.

## Cheat codes

G in the ROM list opens the selected game's codes: type a code and Enter adds it, Up/Down selects,
Del removes, Esc saves and goes back. Each code shows what it decodes to.

- **Game Genie:** 6 or 8 letters (`SXIOPO`). They patch what the CPU reads from ROM: an 8 KB page a
  code applies to (for 8 letter codes, only where the ROM holds the compare value) is copied to
  RAM, patched and mapped in its place, so the CPU's ROM reads stay as fast as without codes. Up
  to six such pages (48 KB of RAM) at a time.
- **Action Replay:** a RAM address and value in hex, `AAAA:VV` (also `AAAAVV` or `00AAAAVV`),
  written at the start of every frame.

A game's codes are on when it starts; F10 switches them off and on (shown briefly on screen). They
are saved in SPIFFS next to the save games, only when changed. Over serial: `g` and a number opens
the editor (characters type, Enter adds, `!` removes, `~` leaves), `c` in a game toggles.

## Per game settings

Settings per game go into the options column of `settings.csv` or into an optional `Game.cfg`
next to `Game.nes` in `roms/`, which is built in with it:

```
# yes/no: battery save RAM, overriding the header (some games use the RAM without a battery)
save = no
# ntsc/pal: TV system, overriding the header and the "(Europe)"-style name tags
region = pal
# 0-100: percent more CPU time per frame, see below
overclock = 50
```

The same settings can go into the options column of `settings.csv`, separated by `;`:
`"Elite (Europe) (En,Fr,De).nes","Elite","elite.png","elite.txt","overclock=100"`. A `.cfg`
wins over the CSV where both set something.

The TV choice in the ROM list (Left/Right), when not Auto, overrides both.

### Overclocking

`overclock=N` (0-100) gives the CPU N percent more cycles per frame, for games that slow down when
busy (Kirby, Elite, shooters). As in Mesen, the extra time is inserted as lines between the
picture and vblank in which only the CPU runs: the picture's timing and the vblank length stay
untouched, and the APU is frozen during them, so the music keeps its tempo. A game that waits
for vblank in a loop just waits longer, so it rarely breaks anything.

Each extra line costs about 23 us on the ESP32: PAL games at 100% use about 17.5 of their 20 ms
per frame. NTSC frames are shorter (16.6 ms), so heavy NTSC games may start skipping frames near
100%; the serial statistics show how close a game gets.

## Fonts

The firmware's two fonts are pictures in `fonts/`, for any paint program (Aseprite with a grid of
the glyph size works well): `font8x8.png` (menu, manuals, key mapper, messages over the game) and
`font4x6.png` (the small labels on the drawn pad). 16 x 16 glyphs in code page 437 order, glyph n
at row n / 16, column n % 16; light pixels are set. The build converts them
(`src/generated/fonts.cpp`); `python3 tools/nesfont.py show fonts/font8x8.png` shows the printable
glyphs in a terminal, `python3 tools/nesfont.py export` writes FabGL's originals back (overwriting
the pictures).

The logo in the ROM list is drawn from the font: a 32 x 16 block of 4 x 2 glyphs, 0xEC-0xEF over
0xFC-0xFF (the bottom right corner of `font8x8.png`). Glyphs that must keep their shape: 0x10 (the
paused game's arrow), 0x18-0x1B (arrows in key names and the help line), 0xFB (a used savestate
slot), and the letters and umlauts the manuals use.

## Build & flash

```sh
pio run -t upload       # firmware with the ROMs (~/.platformio/penv/bin/pio if pio is not on the PATH)
pio device monitor
```

The 512 KB SPIFFS partition is formatted on the first boot and holds the save games.

The firmware is plain ESP-IDF 5.2 (espressif32 6.7.0), without the Arduino core: that saved
155 KB of flash, mostly newlib's full printf/scanf (the IDF uses the nano one in the ESP32's ROM).
The few Arduino APIs the code uses (`Serial`, `String`, `SPIFFS` and `File`, `millis`, the GPIO
calls) are in `src/compat.*`; the IDF's settings in `sdkconfig.defaults` (after changing it, delete
`sdkconfig.pico32` so it is generated again). FabGL 1.0.9 was made for IDF 4.4: the files ESPNES
uses are patched for 5.x, the others are left out of the build (`lib/FabGL/VENDORED.md`). The
first build downloads the ESP-IDF package.

## Layout

- `src/nes/` the emulator core, plain C++ without ESP32 dependencies
- `src/main.cpp` the ESP32 side: VGA, keyboard, ROM list, frame pacing
- `src/compat.*` the Arduino APIs main.cpp uses, on ESP-IDF
- `host/` PC build of the core: nestest check, frame dumps to PPM (`async` draws on a second
  thread like the ESP32 build), sound to WAV (`wav=file.wav`), save RAM in and out (`sram=file`), a trace of PPU register writes per scanline and dot
  (`trace=FROM-TO` frames), RAM bytes at the end or on every change (`ram=`, `watch=`), the
  nametables as text (`ntdump`), a benchmark
- `lib/FabGL/` FabGL 1.0.9, vendored (see `lib/FabGL/VENDORED.md`)
- `fonts/` the fonts as pictures (see "Fonts")
- `roms/` built into the firmware by `tools/embed_roms.py`
- `testroms/` test ROMs for the host harness (nestest, AccuracyCoin)
- `roms-test/` local ROMs for testing with the host harness, not built in and not in git
  (Battletoads and Fire Hawk were checked from there: mappers 7 and 71 work, Battletoads' second
  stage scrolls correctly, Fire Hawk's DMC-IRQ panel is stable)
