# ESPNES

A NES and Famicom emulator for ESP32 boards with FabGL's VGA and PS/2 wiring (LilyGO/TTGO VGA32 and
alikes), built for an ESP32-PICO-D4 board: 4 MB of flash on the chip, **no PSRAM, no SD card**.
The games are built into the firmware.

- Accurate enough for the classics: a cycle exact CPU, a scanline PPU with mid-frame splits,
  the full APU, mappers 0, 1, 2, 4, 5, 7, 71 and 85, NTSC and PAL.
- Menus for a pad alone: a main menu with pictures and facts per game, an in-game system menu,
  manuals, two savestates per game, battery saves, cheat codes with an Action Replay style
  search, a memory editor, a file manager.
- NSF music with all six expansion chips (VRC6, VRC7, FDS, MMC5, Namco 163, Sunsoft 5B).
- The Famicom side: the Family BASIC keyboard, the Data Recorder, the microphone.

Source and more information: [github.com/bastetfurry/espnes](https://github.com/bastetfurry/espnes)

## Getting started

### What it runs on

An ESP32 board with FabGL's VGA (RGB222, 64 colours) and PS/2 keyboard wiring; sound is on the
DAC at GPIO 25. ESPNES was made on an older LilyGO VGA32 with an ESP32-PICO-D4 (4 MB flash, no
PSRAM, no SD slot), so everything fits into the ~300 KB of internal RAM. Real NES pads can be
wired to free GPIOs (see "Real pads").

### Build & flash

```sh
pio run -t upload       # firmware with the games (~/.platformio/penv/bin/pio if pio is not on the PATH)
pio device monitor      # serial console, 115200 baud
```

The first build downloads the ESP-IDF package. The 512 KB SPIFFS partition is formatted on the
first boot; it holds the save games, savestates, cheats and settings.

### Adding games

Every `.nes` and `.nsf` file in `roms/` is built into the firmware: `tools/embed_roms.py` runs
before each build and generates `src/generated/` (an `.incbin` per file and the index the menu
shows). `roms/settings.csv` sets the menu, one game per line, in menu order:

```
"Legend of Zelda, The (Europe).nes","The Legend of Zelda","zelda1.png","zelda1.txt"
```

File name, name shown in the menu, picture, manual, options (see "Per game settings"). A ROM the
CSV does not list is still built in, at the end under its file name, with a warning; a game's
line commented out with `#` leaves that game out of the build. A malformed line (a missing quote,
say) is reported and skipped. Changing the games means rebuilding and flashing: nothing is
written to flash at runtime. The firmware partition is 3.4 MB, so about 2.7 MB of games fit next
to the code.

No commercial ROMs are included. Some that can be had legally:

- **Elite**: Ian Bell, one of its two authors, offers the never-published US (NTSC) version as a
  free download on [his NES Elite page](http://www.iancgbell.clara.net/elite/nes/index.htm)
  (plain HTTP only). MMC1 with battery save. The European cartridge published by Imagineer is a
  different matter and not covered by that release.
- **nestest** by kevtris, in `testroms/nestest/`: a CPU test with a menu.
- **AccuracyCoin** by 100th_Coin (MIT, [github.com/100thCoin/AccuracyCoin](https://github.com/100thCoin/AccuracyCoin)),
  in `testroms/accuracycoin/`: 146 tests down to single PPU dots and bus cycles (see "Accuracy").
- Homebrew and NSF music from their authors.

## Using it

### Controls

| NES    | Player 1   | Player 2 (numpad) |
|--------|------------|-------------------|
| D-pad  | arrow keys | 8 4 5 6           |
| A      | Ctrl       | .                 |
| B      | Alt        | 0                 |
| Select | Shift      | /                 |
| Start  | Space      | *                 |
| Mic    |            | +                 |

These are the defaults; Config Joypad in the main menu's options changes them (see "Key mapper").
Real NES pads work at the same time as the keys.

Everything works with a pad alone: the menus take the pad's buttons (the hints at the bottom of
each screen name them, the button in red), and **A+B+Select+Start** on pad 1 opens the system
menu in a game. On the keyboard, **SysRq** (Alt+PrintScreen) opens it, the same key as in ESPPC;
Enter works as A and Esc goes back in the menus. Esc in a game belongs to the game.

In a game, also: F1 resets, F2/F3 turn the volume down/up, F5/F6/F7 play, record and stop the
tape, F10 switches the cheats off and on.

Messages show for a few seconds in the top line, with a sound: green and a soft ding for good
news, yellow and a ping for neutral ones, red and a hammered piano for errors. (On the About
screen, 1, 2 and 3 play them.)

### Main menu

The games, with the selected one's picture, its size, mapper and features (save RAM and battery,
CHR RAM, tape, keyboard; for music the sound chips), how many cheats are on and which savestate
slots hold a state. The game paused in the console is green with a play arrow.

Up/Down pick a game, Left/Right turn a page, A starts it (or carries on with the paused one;
starting another asks first, as that ends the paused game). Start opens the options:

- **Manual**: the game's manual, if it has one.
- **Save State / Load State**: two slots per game, Left/Right picks the slot (green: it holds a
  state). Saving needs the game paused; overwriting and loading over a running game ask first.
- **Cheats**: the game's cheat codes (see "Cheat codes").
- **Edit Memory**: the paused game's memory (see "Memory editor").
- **TV**: Auto, NTSC or PAL for the games started from now on. Auto uses the NES 2.0 header,
  then names like "(E)" or "(Europe)", otherwise NTSC.
- **Config Joypad**: the key mapper.
- **Files**: the file manager.
- **About**: the board, the firmware, the licence.
- **Quit Current**: ends the paused game.

### System menu

SysRq or A+B+Select+Start opens it over the game, which waits meanwhile:

- **Manual**, **Save State**, **Load State**, **Cheats**, **Edit Memory** as in the options.
- **Force SRAM Save** (games with a battery): writes the save now, if it changed.
- **Tape** (games that use it: Family BASIC, the games with a tape trap, or whenever a tape is
  in): its place over its length at the top, and play, record, stop and rewind; Left/Right picks
  the button. Play goes on from where the tape was stopped.
- **Input** and **Keymap** (keyboard games): the keyboard as the Famicom's (Key) or the pads
  (Joy), and its layout.
- **Volume**, Left/Right, with a ping to hear it.
- **Reset Console** (asks first) and **Main Menu**, which pauses the game and goes back to the
  main menu. Nothing is copied for that: the console just waits.

B, Esc or SysRq returns to the game.

### Manuals

A, Right or Down turn to the next page, B, Left or Up to the one before, Start or Esc goes back.
How to write one: see "Manuals" under "Game files".

### Savestates

Each game has two slots. A state is 1-12 KB (SMB 1.5 KB, Zelda 12-14 KB): the console's RAM,
nametables, sprites and palette, the cartridge's RAM and CHR RAM, and the registers, compressed
with a small LZ that needs no buffer (`src/nes/state.h`). They are files in SPIFFS next to the
saves (`.st1`, `.st2`). A save is refused when it would leave less than 48 KB of SPIFFS free, so
the file system keeps room to work, and a new state is written to a new file first, so a failed
save keeps the old one.

A state includes the battery backed RAM: after loading one, leaving the game writes that into the
game's save file, as an emulator does. States belong to this version of the emulator; one made by
a version with a different state format is refused (`STATE_VERSION` in `src/nes/nes.cpp`).
Savestates are not made for music.

### Save games

Games with a battery (the header's battery flag, or `save=yes`) keep their 8 KB of save RAM in
SPIFFS. It is loaded when the game starts and written back only if it changed: when the game goes
back to the main menu, or with Force SRAM Save in the system menu. A reset keeps it, as on the
console. Switching the board off in a game loses what happened since the last write, so go back
to the menu (or force the save) first.

Save files are named after the start of the ROM's name plus the CRC32 of its contents, so
renaming a ROM keeps its save. Some games (Zelda) also use that RAM as working memory, so it
changes every session and gets written every time.

### Cheat codes

Cheats opens the game's codes (up to 24): each with a check mark when on, the code in hex and a
comment. Up/Down select, B switches the selected code on and off, B+Right (or Delete) deletes it
(asks first), A edits it, and A on the last line, NEW, adds one. Start or Esc goes back, writing
the list only if it changed. The codes that are on are on when the game starts; F10 switches them
all off and on.

Editing opens a box with the code and a comment (16 characters). It takes the keyboard (Tab
changes the field, Backspace deletes, Enter is OK, Esc aborts) or the pad on a drawn keyboard (A
types the key, B deletes, Select changes the field, the check mark and the cross are OK and
abort). F1 switches between the two; the choice stays until the next power on. A code can be:

- **Game Genie:** 6 or 8 letters (`SXIOPO`), turned into hex on OK. They patch what the CPU reads
  from ROM: an 8 KB page a code applies to (for 8 letter codes, only where the ROM holds the
  compare value) is copied to RAM, patched and mapped in its place, so the CPU's ROM reads stay
  as fast as without codes. Up to six such pages (48 KB of RAM) at a time.
- **Hex:** `AAAA VV` (below `$8000` a RAM address, written at the start of every frame, like the
  Action Replay; from `$8000` a ROM patch as above) or `AAAA VV CC` (a ROM patch only where the
  ROM holds `CC`). Action Replay's `AAAA:VV` and `00AAAAVV` work too.

The codes are kept in SPIFFS next to the save games (`.cht`, a line per code: on, code and
comment, separated by tabs).

### Cheat search

Select in the cheat list opens the search, while the game runs (from the system menu, or from
the main menu for the paused game). It looks at the console's RAM and the save RAM, 10240 bytes,
as an Action Replay does: **New Search** takes all of them, then each step keeps those whose
value now is **equal to** a value (Left/Right sets it), **greater** or **less than last** time,
**changed** or **unchanged**. Between the steps the game runs on: lose a life, then "Less than
last". With 24 or fewer left, **Show Results** lists them with their value at the last step and
now; A makes a cheat of one, the address with its value now, in the entry box.

The search stays until End Search, or until another game is started or the game is quit. It uses
about 11 KB, the memory the tape would use: while it runs the tape does not play, record or load,
and starting one erases a tape that is in (after asking).

### Memory editor

Edit Memory shows the console's memory 8 bytes a line: Console RAM, SRAM, PPU VRAM (the
nametables), PPU Palette, OAM and, for games with it, CHR-RAM; Select picks the region. Left/Right
move a byte, Up/Down a line, B+Up/Down (or Page Up/Down) a page. A edits the selected byte:
Left/Right change it by $01, Up/Down by $10, A writes it, B leaves it as it was. The game goes on
with what was written.

### File manager

Everything in SPIFFS, a line per file with a tag for what it is, whose it is and its size, and at
the top how full SPIFFS is:

| Tag | File | Tag | File |
|---|---|---|---|
| `[SRM]` | save RAM (`.sav`) | `[CHT]` | cheat codes (`.cht`) |
| `[ST1]` `[ST2]` | savestates | `[TP1]`-`[TP4]` | tape trap data |
| `[TMP]` | a state cut off while being written | `[CNF]` | settings (`keys.cfg`, `layout.cfg`) |
| `[???]` | anything else | | |

Files of games that are no longer built in are red: their names carry the CRC of a ROM no game in
the list has. A shows the file in the file viewer (the memory editor's screen, read only; states
are compressed, so they look like noise). Select+A (or Delete) deletes the selected file,
Select+B all files of removed games; both ask first. Select also counts for a second after it was
let go, so the pairs can be pressed one after the other.

### Key mapper

Config Joypad picks the player (1 or 2), then a drawn pad shows the button being set blinking;
press a key for each button in turn (Up, Down, Left, Right, Select, Start, B, A, and for player 2
the Mic). A key another button (of either player) already has swaps with it, Esc cancels. The
keys are the same for all games and kept in SPIFFS (`/keys.cfg`), written only when they change.
Left and right Ctrl/Alt/Shift count as one key, a letter works with Shift held, and a numpad key
is the same with or without Num Lock (but not the same as the arrow keys). Esc, F1-F3, F5-F7, F10,
Scroll Lock and SysRq cannot be used.

Mic is the Famicom's microphone on the second pad: while it is held, `$4016` bit 2 reads noise,
like someone talking into it (the Japanese Zelda's Pols Voice).

Cheap keyboards may not see four letter keys pressed at once, so A+B+Select+Start can fail on
them with letter keys mapped; SysRq always works.

### Keyboard layout

The layout is one of FabGL's: US (the default), UK, German, Italian, Spanish, French, Belgian,
Norwegian or Japanese. It is chosen in the system menu (keyboard games) and kept in SPIFFS
(`/layout.cfg`). Text (cheat comments), the Famicom keyboard and the key mapper's names follow it.
The pads' keys are stored by their place on the keyboard, so a layout change does not move them.

### Real pads

NES pads can be wired to free GPIOs: latch and clock shared by both pads, a data line each. Set
the pins at `PAD_LATCH_GPIO`, `PAD_CLOCK_GPIO` and `PAD_DATA_GPIO` in `src/main.cpp` (-1 = not
connected).

- Power the pads from **3.3 V**, not 5 V: the pad's 4021 runs on it, and its data line then
  cannot put 5 V on the ESP32.
- The data lines need a pull-up, so an unplugged pad reads as no buttons: the firmware turns on
  the internal one. GPIO 34-39 have none and need an external one (10 kΩ to 3.3 V).
- Taken: VGA 4, 5, 15, 18, 19, 21, 22, 23; PS/2 keyboard 32, 33; sound 25. Avoid the strapping
  pins 0, 2 and 12. Left as candidates (on a LilyGO/TTGO VGA32): 13 and 14 (the SD slot's on
  boards that have one), 26 and 27 only if the second PS/2 socket (mouse) stays empty, and 34-39
  for data lines only (inputs, external pull-up).
- NES port, pin 1 GND, 2 clock, 3 latch, 4 data, 7 +V (pins 5 and 6 are the Zapper's); check
  with a meter, clones and extension cables differ.

## Famicom extras

### Family BASIC keyboard

Games with "BASIC" in the file name, or `keyboard=yes` in their options, get the Family BASIC
keyboard: the PC keyboard becomes the Famicom's (`$4016` writes select the row and column,
`$4017` bits 1-4 read the keys, see `src/nes/famikey.h`). Letters, digits and symbols type what
the layout prints on them. The emulator presses or hides the Famicom's Shift as the character
needs, so `;` (Shift+, on QWERTZ) arrives as the Famicom's unshifted `;`. A character the Famicom
lacks (an umlaut) is the Famicom key at that place. AltGr stays the layout's. Only SysRq and
Scroll Lock stay the emulator's; Scroll Lock (or Input in the system menu) switches the keyboard
to the pads and back. The other keys:

| Famicom | PC | Famicom | PC |
|---|---|---|---|
| ESC | Esc, Tab | STOP | Pause/Break |
| CTR | Ctrl | GRPH | Alt (left) |
| KANA | Caps Lock | CLR HOME | Home |
| INS | Insert | DEL | Backspace, Delete |
| ¥ | the key left of 1 | _ | Menu key |
| F1-F8 | F1-F8 | arrows | arrows |

Use Family BASIC V3.0, which has 4 KB for programs (4086 bytes free); V2.x has 2 KB. In an
emulator V3.0 can have 8 KB with three Game Genie codes: `YNXAGVYV` (top of memory `$6FFF` →
`$7FFF`), `EESOAYAY` and `EEKXOIAY` (the bounds check and the memory move, `$7000` → `$8000`).
That gives 8182 bytes free. All three are needed; a real cartridge has 4 KB.

SAVE and LOAD go to the Data Recorder, or through the tape trap (see below), which asks for a
slot (1-4) or the PC and takes no time. Programs move between the board and the PC as text
listings with `tools/fbasic.py`. It reads tapes (Family BASIC's format is the Sharp MZ one: a
header block with type, name and size, then the program as it is in memory at `$6006`) and trap
files (the same header and data), and turns the tokens into text, or text back into tokens:

    python3 tools/fbasic.py send prog.bas          types LOAD and hands BASIC the program (1 s)
    python3 tools/fbasic.py receive prog.bas NAME  types SAVE "NAME" and writes the listing
    python3 tools/fbasic.py list FILE              the files in a tape or trap file, as listings
    python3 tools/fbasic.py export FILE prog.bas
    python3 tools/fbasic.py import prog.bas OUT [NAME]   a tape if OUT ends in .ftap, else a trap file

Without the trap (`tape=none`), `send` falls back to a tape with one-second leaders instead of
nine. Everything a program needs is in the listing: music is PLAY strings, and sprites are the
fixed character sets (CGSET, DEF SPRITE). Screens made with BGTOOL are saved as files of their
own type; `list` shows their names and sizes but does not convert them. Characters outside ASCII
(kana, graphics) are written as `{XX}` in hex. Typing over serial (`T`) waits after each RETURN,
because BASIC misses keys while it takes in a line.

### Data Recorder

The Famicom Data Recorder was a cassette recorder plugged into the Family BASIC keyboard. The
console has no tape logic: a game toggles `$4016` bit 0 itself to write and watches `$4016`
bit 1 to read (while bit 2 is set), each game with its own encoding. ESPNES records the signal's
level changes in CPU cycles and plays them back exactly, so every game works the same way. A tape
image (`.ftap`, see `src/nes/tape.h`) stores runs of square-wave periods; games time their bits
with counted loops, so a few periods repeat exactly. Mach Rider's 22 second course save takes
1.5 KB.

The tape is in the system menu (play, record, stop, rewind) and on F5/F6/F7. It is held in RAM,
up to 32 KB, and stays in across games like a cassette left in the recorder. The PC keeps the
tapes: when a recording stops, the board sends it over serial, and `tools/tape.py` saves it and
puts tapes back in:

    python3 tools/tape.py record course.ftap     records; Enter stops and saves
    python3 tools/tape.py take course.ftap       saves the next tape recorded on the board
    python3 tools/tape.py play course.ftap       puts it in and plays it
    python3 tools/tape.py put course.ftap        only puts it in (then play on the board)
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

Family BASIC (V2.0a, V2.1a, V3.0 and the Playbox BASIC prototype) keeps its own data at `$6000`,
so it can't use the VC's call. Its five tape routines (write header, write data, read header,
read data, verify) are patched to `lda #OP / jmp STUB`. The stub writes the operation to `$4018`
(the APU's unused test range) and waits while `$4018` reads `$80` (`Nes::portCall`). The file is
BASIC's own: the 128-byte header at `$0500` and the data from its load address, so BGTOOL screens
go the same way as programs, and with the three Game Genie codes above, programs of more than
4 KB too.

When the game saves or loads, a line over the picture asks where: 1-4 picks a slot in SPIFFS
(`.tp1` to `.tp4`, listed in the file manager), P picks the PC, and Esc cancels. For the PC,
`python3 tools/tape.py file course.bin` takes the game's next save or answers its next load. Mach
Rider's data is 320 bytes (`$0600`-`$063F` and `$0700`-`$07FF`), at the same addresses in every
version, so a course moves between them. `python3 tools/tapetrap.py ROM` shows what is found.

## NSF music

NSF files (`roms/*.nsf`, listed in `settings.csv` like games) play as music: the screen shows the
title, artist, copyright, the picture, a level bar per channel (how far each one's output swings,
falling back slowly), the song number and the play time. Left/Right changes the song, Start
pauses, and the system menu works as in games; reset starts the song again.

The player (`src/nes/nsf.*`) calls the NSF's INIT with the song in A and PAL in X, then PLAY at
the NSF's play rate. Both return to `$4100`, which reads as `JMP $4100`. While the CPU waits
there, the console skips the time, so music costs 1-4 ms of the 16.6 ms per frame. The NSF stays
in flash. Its banks are copied into 32 KB of RAM at `$8000`-`$FFFF` (4 KB banks through
`$5FF8`-`$5FFF`, and `$5FF6`-`$5FF7` for FDS music, which also uses `$6000`-`$7FFF` as RAM).

Sound chips besides the 2A03:

- **Konami VRC6** (`src/nes/vrc6_audio.*`): two pulse channels with 8 duty steps and a sawtooth,
  at `$9000`-`$B002`.
- **Konami VRC7** (`src/nes/vrc7_audio.*`): six FM channels. The OPLL inside is Yamaha's cut-down
  OPL2, so its writes are translated into an OPL2's, which DBOPL from DOSBox-X synthesizes at
  24858 Hz (`src/nes/opl`, by way of ESPPC).
- **FDS** (`src/nes/fds_audio.*`): a 64-step wavetable, the pitch modulator, and the volume and
  modulation envelopes, mixed at up to 2.4 times a pulse channel and filtered at about 2 kHz like
  the real one.
- **MMC5**: two pulse channels like the 2A03's without a sweep, clocked by its own 240 Hz counter,
  and an 8 bit PCM, at `$5000`-`$5015`; the NSF mapper adds its multiplier and 1 KB ExRAM.
- **Namco 163** (`src/nes/n163_audio.*`): up to 8 wavetable channels in 128 bytes of sound RAM
  (`$F800` address, `$4800` data), updated one every 15 cycles in turn and mixed as the sum of
  their outputs divided by their number, about 12 dB above a 2A03 pulse.
- **Sunsoft 5B** (`src/nes/s5b_audio.*`): a YM2149F at half an AY's speed, three square channels,
  noise and a 32-step envelope through a logarithmic DAC, written through `$C000`/`$E000`.

## Game files

### Pictures

The picture is a PNG in `roms/`, scaled to fit 64x64 (aspect kept, black bars) in NES palette
colours by `tools/nesimage.py` (plain Python, no Pillow needed). Screenshots and other pixel art
(up to 1024 colours) keep their own colours: each 64x64 pixel takes the colour of its source
block closest to the block's average, without dithering, so flat areas stay flat and edges crisp.
Photos and box art are averaged and Floyd-Steinberg dithered. Text smaller than about 16 pixels
does not survive the scaling; a picture cropped to the logo works better than a full title
screen. `python3 tools/nesimage.py picture.png preview.ppm` shows what a picture will look like.

Converted pictures are cached in `src/generated/pics/`. In the firmware they are raw deflate
compressed (about 300-700 bytes instead of 4 KB) and unpacked with the inflate in the ESP32's mask
ROM, so the decoder costs no flash. A missing picture is a build warning and an empty square in
the menu.

### Manuals

A manual is a text file in `roms/` of teletext style pages, 32 columns by 25 lines:

```
[PAGESTART=1]
The Legend of Zelda \[PAL\]

[color=red]Story
[color=white]The kingdom of Hyrule has fallen
[PAGEEND]
```

`[color=...]` (black red green yellow blue magenta cyan white) lasts until the next one; every
page starts in white. `\[`, `\]` and `\\` are literal brackets and backslashes. A line that holds
only tags gives no line on the screen. Longer lines and pages are cut with a build warning,
nothing is wrapped. Umlauts work (the font is code page 437). On the device it is stored as a
cut-down VT100 form (see `tools/nesmanual.py`); `python3 tools/nesmanual.py roms/zelda1.txt`
previews a manual in the terminal with its colours and warnings.

### Per game settings

Settings per game go into the options column of `settings.csv`, separated by `;`
(`"Elite (Europe) (En,Fr,De).nes","Elite","elite.png","elite.txt","overclock=100"`), or into an
optional `Game.cfg` next to `Game.nes` in `roms/`, which is built in with it. A `.cfg` wins over
the CSV where both set something:

```
# yes/no: battery save RAM, overriding the header (some games use the RAM without a battery)
save = no
# ntsc/pal: TV system, overriding the header and the "(Europe)"-style name tags
region = pal
# 0-100: percent more CPU time per frame, see below
overclock = 50
# yes/no: the Family BASIC keyboard (else: "BASIC" in the file name)
keyboard = yes
# machrider/none: the tape trap (else: found from the game's code)
tape = none
```

The TV choice in the options, when not Auto, overrides both.

`overclock=N` (0-100) gives the CPU N percent more cycles per frame, for games that slow down when
busy (Kirby, Elite, shooters). As in Mesen, the extra time is inserted as lines between the
picture and vblank in which only the CPU runs: the picture's timing and the vblank length stay
untouched, and the APU is frozen during them, so the music keeps its tempo. A game that waits for
vblank in a loop just waits longer, so it rarely breaks anything. Each extra line costs about
23 us on the ESP32: PAL games at 100% use about 17.5 of their 20 ms per frame. NTSC frames are
shorter (16.6 ms), so heavy NTSC games may start skipping frames near 100%; the serial statistics
show how close a game gets.

## How it works

### Emulation

- **CPU:** 2A03, all official and the stable unofficial opcodes, exact cycle counts. Passes
  nestest's automated mode cycle for cycle (`make -C host test`).
- **PPU:** scanline renderer with the loopy scroll model, so `$2005`/`$2006` splits work; sprite 0
  hit is timed to the dot, 8 sprites per line, 8x16 sprites, left column clipping, greyscale.
  Sprite evaluation follows the hardware byte by byte: it starts at OAMADDR (which rendering
  resets to 0 every line), re-aligns a misaligned address, and has the overflow flag bug. NTSC
  (262 lines, 3 dots per CPU cycle) and PAL (312 lines, 3.2 dots) timing.
- **Mappers:** 0 (NROM), 1 (MMC1, including SUROM's 512 KB), 2 (UxROM), 4 (MMC3, with its
  scanline IRQ), 5 (MMC5: all PRG/CHR modes, separate sprite/background CHR sets for 8x16
  sprites, ExRAM as nametable, fill mode, scanline IRQ, multiplier; not its extended attributes,
  split screen or sound), 7 (AxROM, with AOROM bus conflicts), 71 (Camerica/Codemasters, with Fire
  Hawk's mirroring), 85 (VRC7: Lagrange Point, with its FM sound and the VRC IRQ counter, computed
  lazily to the CPU cycle). The cartridge layer works through bank pointer tables, so a mapper
  only repoints 8 KB PRG and 1 KB CHR pages.
- **APU:** both pulse channels with sweep and envelope, triangle, noise, DMC sample playback with
  its IRQ, the frame counter (4 and 5 step, frame IRQ), `$4015` status. DMC DMA cycle stealing is
  approximated.
- **Not yet:** colour emphasis, PPU open bus decay.

Super Mario Bros. runs at full speed with no frame skipping, using ~8.8 ms of the 16.6 ms frame.

### Accuracy

On AccuracyCoin, ESPNES passes 103 or 104 of 146 (2026-10-14), nothing skipped: which one depends
on the frame Start is pressed in, since that sets the phase of the free running DMC against the
tests, and the DMC DMA's cycle stealing is only approximated (pages 12-16 change, e.g. "DMA + open
bus" and "DMA + $2007 read"). The PC harness and the board give the same results for the same
frame. The CPU and opcode pages pass; the failures are the dot exact PPU, DMA and APU timing a
scanline renderer does not model. For comparison, [doclic's leaderboard](https://doclic.eu/neslead.htm)
lists Nintendo's NSO emulator at 97 of 140 on an older version of the suite.

### Cores

Core 1 runs the CPU and the PPU's timing; core 0 draws the picture. At the start of each visible
line core 1 snapshots the PPU state the line depends on (scroll, control, mask, palette, bank
pointers) and moves on; core 0 draws from those snapshots straight into the VGA frame buffer.
Sprite 0 hit is still decided on core 1 (it only needs the 8 pixels under sprite 0), so the CPU
sees it at the right dot. Before vblank, when games rewrite VRAM and OAM, core 1 waits for core 0
to finish the frame; in practice it never has to.

The drawing code lives in flash on purpose, not IRAM: both cores fetching instructions from IRAM
at once slowed each other down measurably, while flash code runs from each core's own cache.

### Video

320x240 at 60 Hz with the 256x240 picture centred, black borders left and right. The PPU writes
raw pixels straight into FabGL's DMA line buffers, so the output costs no interrupt time. The
board has 2 bits per colour channel, so the 64 NES colours are approximated with RGB222. PAL games
are emulated at 50 frames per second on the 60 Hz signal.

The menus are a grid of 32 x 30 characters (`src/ui.*`): a screen writes what it wants into the
grid, and only the cells that differ from what is on the monitor are drawn, straight into the
frame buffer, so nothing flickers and a box over a screen needs nothing saved under it.

### Sound

The APU runs on core 1 with the CPU, lazily: it catches up whenever an APU register is touched and
once per scanline. Output is 22050 Hz on the DAC (GPIO 25); each sample averages the channels
over its time span, so high notes do not alias much. The samples go through a ring buffer to
FabGL's SoundGenerator, which feeds the DAC from an I2S interrupt. The emulation is paced by a
timer and the DAC by the I2S clock, so the APU's sample rate is nudged by up to 0.5% to keep about
three frames buffered. The menus' sounds are mixed in by the same interrupt.

### Games in flash

Constant data in flash is mapped into the address space through the cache, so the emulator reads
a ROM in place. Nothing is copied to RAM, which a 128 KB game would not fit into anyway, and
nothing is written to flash at runtime except the save games and settings in SPIFFS, and those
only when they change. Reading PRG from flash through the cache costs nothing measurable (SMB:
8.7 ms per frame either way).

### Fonts

The firmware's two fonts are pictures in `fonts/`, for any paint program (Aseprite with a grid of
the glyph size works well): `font8x8.png` (menus, manuals, messages over the game) and
`font4x6.png` (the small labels on the drawn pad). 16 x 16 glyphs in code page 437 order, glyph n
at row n / 16, column n % 16; light pixels are set. The build converts them
(`src/generated/fonts.cpp`); `python3 tools/nesfont.py show fonts/font8x8.png` shows the printable
glyphs in a terminal.

Glyphs the menus use and that must keep their meaning: the logo, a 4 x 2 block at 0xEC-0xEF over
0xFC-0xFF; 0xEA (music) and 0xEB (a game) as the list's icons; 0x10 (play, the paused game), 0x07
(record), 0xDB (stop), 0xAE (rewind); 0x1E/0x1F (more above/below); 0x2A (a cross: off, empty) and
0xFB (a check mark: on, used); the box drawing characters of code page 437; and the letters and
umlauts the manuals use. (0x2A is `*` in code page 437, so a `*` typed in a comment shows as a
cross.)

### ESP-IDF

The firmware is plain ESP-IDF 5.2 (espressif32 6.7.0), without the Arduino core: that saved
155 KB of flash, mostly newlib's full printf/scanf (the IDF uses the nano one in the ESP32's ROM).
The few Arduino APIs the code uses (`Serial`, `String`, `SPIFFS` and `File`, `millis`, the GPIO
calls) are in `src/compat.*`; the IDF's settings in `sdkconfig.defaults` (after changing it,
delete `sdkconfig.pico32` so it is generated again). FabGL 1.0.9 was made for IDF 4.4: the files
ESPNES uses are patched for 5.x, the others are left out of the build (`lib/FabGL/VENDORED.md`).

## Development

### Serial console

At 115200 baud the board prints the game list at boot and frame timings every 5 seconds (emulation
time on core 1, drawing time on core 0, how long core 1 waited for it). It also takes commands,
for testing without a keyboard:

- A number starts that game from the list.
- `a b e s u d l r` are the pad's buttons (A, B, Select, Start and the directions), in the menus
  as in games; taps close together add up (`abes` is the system menu's combination).
- `Z` (or Ctrl+Z) sends a screenshot: `tools/screenshot.py [--keys ...] out.png` sends the keys,
  then makes a PNG of the screen.
- In a game: `q` goes back to the main menu, `y` opens the system menu, `c` switches the cheats,
  `P`, `R`, `X` play, record and stop the tape, `U` puts one in (see Data Recorder), `T` and a line
  types it on the Famicom keyboard (`|` is RETURN); while a tape trap asks, `1`-`4`, `p` and `~`
  answer it.
- In the cheat entry box (keyboard mode): characters type, Enter is OK, `~` aborts, `|` changes
  the field, `^` deletes, `!` switches to the drawn keyboard.
- In the key mapper: `1`/`2` picks the player, then a letter, digit or space for the next button,
  `^` the player's defaults, `~` cancels.

### Host harness

`host/` builds the emulator core on a PC (`make -C host`, `make -C host test` runs nestest against
its golden log). `host/nestool run ROM FRAMES out.ppm` runs a game and dumps frames; options:
`async` draws on a second thread like the ESP32 build, `wav=file.wav` writes the sound,
`sram=file` save RAM in and out, `trace=FROM-TO` the PPU register writes per scanline and dot,
`ram=`/`watch=` RAM bytes at the end or on every change, `ntdump` the nametables as text,
`statetest=F` saves a state at frame F, runs on, loads it and checks the frames come out the same,
`type=F:TEXT` types on the Family BASIC keyboard, `taperec=FILE@F`/`tapeplay=FILE@F` the tape,
`song=N` and `nofds` for music, and a benchmark.

### Layout

- `src/nes/` the emulator core, plain C++ without ESP32 dependencies
- `src/main.cpp` the ESP32 side: VGA, keyboard and pads, the menus, frame pacing
- `src/ui.*` the menus' text grid, messages and sounds
- `src/compat.*` the Arduino APIs main.cpp uses, on ESP-IDF
- `host/` PC build of the core (see "Host harness")
- `tools/` the build's converters (ROMs, pictures, manuals, fonts) and the PC side of the tape,
  Family BASIC and screenshots
- `lib/FabGL/` FabGL 1.0.9, vendored (see `lib/FabGL/VENDORED.md`)
- `fonts/` the fonts as pictures (see "Fonts")
- `roms/` built into the firmware by `tools/embed_roms.py`
- `testroms/` test ROMs for the host harness (nestest, AccuracyCoin)
- `roms-test/` local ROMs for testing with the host harness, not built in and not in git
- `MenuMockup.txt` the menus as designed, 32 x 30 characters each

## Licence and credits

ESPNES is by Bastet and Claude, released under the GNU General Public License version 3 (see
`LICENSE`). It is free software: if you paid extra for it, ask for a refund.

It uses:

- [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio (GPL): VGA, PS/2 and sound,
  vendored in `lib/FabGL/` with changes for ESP-IDF 5.
- DBOPL, the OPL2 emulator from [DOSBox](https://www.dosbox.com/) / DOSBox-X (GPL), by way of
  ESPPC, for the VRC7's FM sound (`src/nes/opl/`).
- nestest by kevtris and AccuracyCoin by 100th_Coin (MIT) as test ROMs.
- [Mesen](https://www.mesen.ca/) and FCEUX as references to compare against while debugging.
