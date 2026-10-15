// ESPNES - NES emulator on an ESP32 with FabGL VGA output and a PS/2 keyboard as the pad.
//
// Video: 320x240 at 60 Hz with the NES's 256x240 picture centred (black side borders). FabGL's
// VGAController sends the frame buffer by DMA straight from its line buffers, and the PPU writes
// its finished lines into those buffers as raw VGA pixels, so the picture costs no interrupt time.
//
// Cores: the CPU, the PPU's timing and the APU run in the Arduino loop task on core 1; the lines
// are drawn by a task on core 0 from per-line snapshots (see Nes::setRenderKick).
//
// Sound: the APU's samples go through a ring buffer to FabGL's SoundGenerator, which feeds the
// DAC on GPIO 25 from an I2S interrupt (on core 0). The emulation is paced by a timer and the DAC
// by the I2S clock, so their rates drift apart a little: the APU's sample rate is nudged by the
// buffer's fill level to keep it from running dry or overflowing.
//
// Keys: arrows = D-pad, Ctrl = A, Alt = B, Shift = Select, Space = Start; player 2 and the Famicom
//       mic on the numpad (all changed with K in the ROM list); real NES pads on GPIOs (see
//       PAD_LATCH_GPIO). Esc = back to the ROM list, F1 = reset, F2/F3 = volume down/up, F10 = cheats on/off.
// In the ROM list, G edits the selected game's Game Genie / Action Replay codes.
// Esc pauses the game and shows the ROM list; Enter on the paused game carries on, S saves its
// state in one of two slots per game, L loads one (see "savestates").
// Serial console: a number starts that ROM from the list, 'k' sets the keys, 'q' leaves the game, and
// a b e s u d l r tap A, B, sElect, Start and the directions.

#include <Arduino.h>
#include <SPIFFS.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "fabgl.h"
#include "esp_timer.h"
#include "esp32/rom/crc.h"
#include "esp32/rom/miniz.h"

#include "nes/nes.h"
#include "nes/palette.h"
#include "romindex.h"


static fabgl::VGAController   VGA;
static fabgl::PS2Controller   PS2;
static Nes                    nes;

static uint8_t                s_raw[64];        // NES colour -> raw VGA pixel (RGB222 + sync bits)
static uint8_t *              s_lines[240];     // the frame buffer's line pointers


static void overlayHook(void *, int y, uint8_t * line);

static TaskHandle_t s_renderTask;

static void renderTask(void *)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    nes.drawQueuedLines();
  }
}


static void kickRenderer(void *)
{
  xTaskNotifyGive(s_renderTask);
}


static void initVideo()
{
  // the canvas (ROM list) is the only user of FabGL's primitive queue: keep it small, it is RAM
  fabgl::BitmappedDisplayController::queueSize = 128;
  VGA.begin();                                       // FabGL's default pins, 64 colours
  VGA.setResolution(QVGA_320x240_60Hz, 256, 240);
  for (int i = 0; i < 64; ++i) {
    uint32_t c = NES_PALETTE_RGB[i];
    // to 2 bits per channel, rounding
    auto q = [](uint32_t v) { return (uint8_t)(((v & 0xff) * 3 + 127) / 255); };
    s_raw[i] = VGA.createRawPixel(RGB222(q(c >> 16), q(c >> 8), q(c)));
  }
  for (int y = 0; y < 240; ++y)
    s_lines[y] = VGA.getScanline(y);
  // the PPU draws raw pixels straight into the frame buffer, in the DMA's byte order
  nes.setVideoOutput(s_lines, s_raw, true);

  // Priority above the idle task but below FabGL's keyboard task (5). Between frames it blocks,
  // so core 0's idle task still runs and its watchdog stays quiet.
  xTaskCreatePinnedToCore(renderTask, "render", 3072, nullptr, 2, &s_renderTask, 0);
  nes.setRenderKick(kickRenderer, nullptr);
  nes.setLineHook(overlayHook, nullptr);
}


///////////////////////////////////////////////////////////////////////////////
// sound

static constexpr int      AUDIO_RATE = 22050;
static constexpr uint32_t RING_SIZE  = 4096;               // power of two
static int8_t             s_ring[RING_SIZE];
static std::atomic<uint32_t> s_ringHead(0);                // written by core 1
static std::atomic<uint32_t> s_ringTail(0);                // written by the sound interrupt
static int8_t             s_lastSample;
static int                s_volume = 3;                    // 0-8

// FabGL calls getSample() from its I2S interrupt for every DAC sample
class ApuStream : public fabgl::WaveformGenerator {
public:
  void setFrequency(int value) override { }
  int  getSample() override;
};

int IRAM_ATTR ApuStream::getSample()
{
  uint32_t tail = s_ringTail.load(std::memory_order_relaxed);
  if (tail != s_ringHead.load(std::memory_order_acquire)) {
    s_lastSample = s_ring[tail & (RING_SIZE - 1)];
    s_ringTail.store(tail + 1, std::memory_order_release);
  }
  // when the buffer runs dry, hold the last level: that is silent, a jump to 0 would click
  return s_lastSample;
}

static fabgl::SoundGenerator * s_soundGen;
static ApuStream               s_apuStream;

static void initSound()
{
  s_soundGen = new fabgl::SoundGenerator(AUDIO_RATE, GPIO_NUM_25, fabgl::SoundGenMethod::DAC);
  s_soundGen->setVolume(127);
  s_soundGen->attach(&s_apuStream);
  s_apuStream.setVolume(127);
  s_apuStream.enable(true);
  s_soundGen->play(true);
}

static uint32_t ringFill()
{
  return s_ringHead.load(std::memory_order_relaxed) - s_ringTail.load(std::memory_order_acquire);
}

// Moves the frame's samples to the ring, scaled to the DAC's 8 bits.
static void pushSamples()
{
  int n = nes.apu.sampleCount();
  int16_t const * src = nes.apu.samples();
  uint32_t head = s_ringHead.load(std::memory_order_relaxed);
  uint32_t space = RING_SIZE - ringFill();
  if ((uint32_t) n > space)
    n = space;                                             // overflow: drop the rest
  for (int i = 0; i < n; ++i) {
    int v = (src[i] * s_volume) >> 8;
    s_ring[(head + i) & (RING_SIZE - 1)] = v > 127 ? 127 : (v < -127 ? -127 : v);
  }
  s_ringHead.store(head + n, std::memory_order_release);
  nes.apu.takeSamples();
}


///////////////////////////////////////////////////////////////////////////////
// ROM list

enum class RegionChoice { Auto, NTSC, PAL };

static RegionChoice s_regionChoice = RegionChoice::Auto;


static void listRoms()
{
  Serial.println("ROMs (send the number over serial to start one):");
  for (int i = 0; i < g_romCount; ++i)
    Serial.printf("  %d: %s (%s, %u bytes%s%s)\n", i, g_roms[i].title, g_roms[i].name, (unsigned) g_roms[i].size,
                  g_roms[i].picture ? ", picture" : "", g_roms[i].manual ? ", manual" : "");
}


static fabgl::Canvas * s_canvas;

static String savePathFor(RomBlob const & rom, char const * ext = ".sav");
static void   editCheats(RomBlob const & rom);
static char const * editPadKeys();
static char const * fileManager();
static bool   slotUsed(RomBlob const & rom, int slot);
static char const * saveSlot(int idx, int slot);
static void   applyCheats(RomBlob const & rom);

// The game in the console while the menu shows: Esc pauses it (nothing is copied, the console just
// waits), Enter on it carries on. Starting or loading another game ends it. -1: none.
static int s_paused = -1;

// menu layout: the list on the left, a PICTURE_SIZE square picture on the right
static constexpr int LIST_Y = 20;
static constexpr int PIC_X  = 256 - 4 - PICTURE_SIZE;
static constexpr int PIC_Y  = LIST_Y;

// What the menu shows right now, so a key press redraws only what changed instead of the whole
// screen (which flickers). Any other screen (a game, a manual) invalidates it.
static struct {
  bool         valid = false;
  int          selected, top;
  RegionChoice region;
  char const * message;
} s_shown;


static void drawRow(int idx, int top, bool highlighted)
{
  fabgl::Canvas & cv = *s_canvas;
  int y = LIST_Y + (idx - top) * 9;
  cv.setBrushColor(highlighted ? Color::Blue : Color::Black);
  cv.fillRectangle(0, y - 1, PIC_X - 6, y + 7);
  String n = g_roms[idx].title;
  if (n.length() > 21)
    n = n.substring(0, 21);
  if (idx == s_paused) {
    cv.setPenColor(Color::BrightGreen);
    cv.drawText(2, y, "\x10");         // the paused game: a play arrow
  }
  cv.setPenColor(highlighted ? Color::BrightWhite : Color::White);
  cv.drawText(12, y, n.c_str());       // glyphs fill their background with the brush colour
  cv.setBrushColor(Color::Black);
}


static void drawRegion()
{
  static char const * regionNames[] = { "Auto", "NTSC", "PAL " };
  char buf[16];
  snprintf(buf, sizeof(buf), "TV: %s", regionNames[(int)s_regionChoice]);   // fixed width
  s_canvas->setPenColor(Color::White);
  s_canvas->drawText(256 - 8 - 8 * 8, 4, buf);
}


static void drawMessage(char const * message)
{
  fabgl::Canvas & cv = *s_canvas;
  cv.setBrushColor(Color::Black);
  cv.fillRectangle(0, 200, 255, 209);
  if (message) {
    Serial.printf("Menu: %s\n", message);
    cv.setPenColor(Color::BrightRed);
    cv.drawText(8, 201, message);
  }
}


// under the picture: which of the game's savestate slots hold a state
static void drawSlots(int idx)
{
  fabgl::Canvas & cv = *s_canvas;
  for (int slot = 1; slot <= 2; ++slot) {
    bool used = slotUsed(g_roms[idx], slot);
    char text[12];
    snprintf(text, sizeof(text), "Slot %d %c", slot, used ? 0xfb : '-');    // a check mark
    cv.setPenColor(used ? Color::BrightGreen : Color::BrightBlack);
    cv.drawText(PIC_X, PIC_Y + PICTURE_SIZE + 6 + (slot - 1) * 10, text);
  }
}


// Unpacks a picture with the inflate in the ESP32's ROM (no code of ours in flash). Its decoder
// state is about 11 KB, too much for the stack: it and the picture are on the heap only while
// drawing.
struct PictureUnpack {
  tinfl_decompressor state;
  uint8_t            pixels[PICTURE_SIZE * PICTURE_SIZE];
};

static bool unpackPicture(RomBlob const & rom, PictureUnpack * u)
{
  if (!rom.picture)
    return false;
  tinfl_init(&u->state);
  size_t inSize = rom.pictureSize, outSize = sizeof(u->pixels);
  tinfl_status st = tinfl_decompress(&u->state, rom.picture, &inSize, u->pixels, u->pixels, &outSize,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return st == TINFL_STATUS_DONE && outSize == sizeof(u->pixels);
}


// straight into the frame buffer; call once the canvas has finished drawing
static void drawPicture(int idx)
{
  auto * u = (PictureUnpack *) malloc(sizeof(PictureUnpack));
  uint8_t const * pic = u && unpackPicture(g_roms[idx], u) ? u->pixels : nullptr;
  for (int y = 0; y < PICTURE_SIZE; ++y) {
    uint8_t * line = s_lines[PIC_Y + y];
    for (int x = 0; x < PICTURE_SIZE; ++x)   // the DMA's byte order
      line[(PIC_X + x) ^ 2] = s_raw[pic ? pic[y * PICTURE_SIZE + x] & 0x3f : 0x0f];
  }
  free(u);
}


static void drawMenu(int selected, int top, int visible, char const * message)
{
  fabgl::Canvas & cv = *s_canvas;
  bool full = !s_shown.valid || top != s_shown.top;
  bool moved = full || selected != s_shown.selected;

  if (full) {
    cv.setBrushColor(Color::Black);
    cv.clear();
    cv.setPenColor(Color::BrightYellow);
    cv.drawText(8, 4, "ESPNES");
    drawRegion();
    if (!g_romCount) {
      cv.setPenColor(Color::BrightRed);
      cv.drawText(8, 40, "No ROMs built in.");
      cv.setPenColor(Color::White);
      cv.drawText(8, 56, "Put .nes files in roms/ and");
      cv.drawText(8, 68, "run pio run -t upload");
    } else {
      for (int i = 0; i < visible && top + i < g_romCount; ++i)
        drawRow(top + i, top, top + i == selected);
      // the picture box, empty when the game has no picture
      cv.setPenColor(Color::BrightBlack);
      cv.drawRectangle(PIC_X - 1, PIC_Y - 1, PIC_X + PICTURE_SIZE, PIC_Y + PICTURE_SIZE);
    }
    drawMessage(message);
    cv.setPenColor(Color::BrightBlack);
    cv.drawText(4, 213, "Enter:play \x1b\x1a:TV M:man G:codes");
    cv.drawText(4, 222, "S/L:save/load K:keys F:files");
    cv.drawText(4, 231, "F1:rst F2/3:vol F10:cheat");
  } else {
    if (moved) {
      drawRow(s_shown.selected, top, false);
      drawRow(selected, top, true);
    }
    if (s_regionChoice != s_shown.region)
      drawRegion();
    if (message != s_shown.message)
      drawMessage(message);
  }
  cv.waitCompletion();

  if (g_romCount && moved) {
    drawSlots(selected);
    cv.waitCompletion();
    drawPicture(selected);
  }

  s_shown.valid    = true;
  s_shown.selected = selected;
  s_shown.top      = top;
  s_shown.region   = s_regionChoice;
  s_shown.message  = message;
}


// The manual viewer: a teletext style page per screen, Left/Right or Page Up/Down to turn pages,
// Esc (or M, Tab) to go back. Over serial: n(ext), p(revious), x to leave.
static void showManual(RomBlob const & rom)
{
  static const Color colours[8] = { Color::Black, Color::BrightRed, Color::BrightGreen, Color::BrightYellow,
                                    Color::BrightBlue, Color::BrightMagenta, Color::BrightCyan, Color::BrightWhite };
  // the pages, found by their form feeds
  std::vector<char const *> pages;
  pages.push_back(rom.manual);
  for (char const * p = rom.manual; *p; ++p)
    if (*p == '\f')
      pages.push_back(p + 1);

  fabgl::Canvas & cv = *s_canvas;
  auto kb = PS2.keyboard();
  int page = 0;

  // the frame of the viewer, drawn once; turning a page then overwrites the text in place,
  // every line padded to the full width, which does not flicker like clearing the screen
  cv.setBrushColor(Color::Black);
  cv.clear();
  cv.setPenColor(Color::BrightCyan);
  String title = rom.title;
  if (title.length() > 24)
    title = title.substring(0, 24);
  cv.drawText(0, 4, title.c_str());
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 230, "L/R:page  Esc:back");

  for (;;) {
    char num[12];
    snprintf(num, sizeof(num), "%7s", (String(page + 1) + "/" + String((int) pages.size())).c_str());
    cv.setPenColor(Color::BrightYellow);
    cv.drawText(256 - 7 * 8, 4, num);

    // the page: runs of one colour at a time, then spaces to the end of the line
    char const * p = pages[page];
    bool pageDone = false;
    Color colour = Color::BrightWhite;
    char run[MANUAL_COLS + 1];
    for (int row = 0; row < MANUAL_ROWS; ++row) {
      int y = 20 + row * 8;
      int col = 0, len = 0, runCol = 0;
      if (!pageDone) {
        for (; *p && *p != '\n' && *p != '\f'; ++p) {
          if (*p == 0x1b && p[1]) {
            if (len) {
              run[len] = 0;
              cv.setPenColor(colour);
              cv.drawText(runCol * 8, y, run);
              len = 0;
            }
            colour = colours[(p[1] - '0') & 7];
            ++p;
            continue;
          }
          if (col < MANUAL_COLS) {
            if (!len)
              runCol = col;
            run[len++] = *p;
            ++col;
          }
        }
        if (len) {
          run[len] = 0;
          cv.setPenColor(colour);
          cv.drawText(runCol * 8, y, run);
        }
        if (*p == '\n')
          ++p;
        else
          pageDone = true;     // '\f' or the end: the rest of the rows are blank
      }
      if (col < MANUAL_COLS) {
        memset(run, ' ', MANUAL_COLS - col);
        run[MANUAL_COLS - col] = 0;
        cv.drawText(col * 8, y, run);
      }
    }
    cv.waitCompletion();

    // keys
    int step = 0;
    while (!step) {
      if (Serial.available()) {
        int c = Serial.read();
        if (c == 'x')
          return;
        step = c == 'n' ? 1 : c == 'p' ? -1 : 0;
        continue;
      }
      if (!kb->virtualKeyAvailable()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      bool down;
      auto vk = kb->getNextVirtualKey(&down, 0);
      if (!down)
        continue;
      switch (vk) {
        case fabgl::VK_ESCAPE: case fabgl::VK_m: case fabgl::VK_M: case fabgl::VK_TAB:
          return;
        case fabgl::VK_RIGHT: case fabgl::VK_KP_RIGHT: case fabgl::VK_PAGEDOWN: case fabgl::VK_KP_PAGEDOWN:
        case fabgl::VK_DOWN:  case fabgl::VK_KP_DOWN:  case fabgl::VK_SPACE:
          step = 1;
          break;
        case fabgl::VK_LEFT:  case fabgl::VK_KP_LEFT:  case fabgl::VK_PAGEUP:   case fabgl::VK_KP_PAGEUP:
        case fabgl::VK_UP:    case fabgl::VK_KP_UP:
          step = -1;
          break;
        default:
          break;
      }
    }
    page = std::max(0, std::min((int) pages.size() - 1, page + step));
  }
}


// Asks for a savestate slot in the message line: 1 or 2, 0 for Esc. Over serial: 1, 2 or ~.
static int askSlot(char const * question)
{
  drawMessage(question);
  s_shown.message = question;         // so the menu redraws its message line afterwards
  s_canvas->waitCompletion();
  auto kb = PS2.keyboard();
  for (;;) {
    if (Serial.available()) {
      int c = Serial.read();
      if (c == '1' || c == '2')
        return c - '0';
      if (c == '~')
        return 0;
      continue;
    }
    if (!kb->virtualKeyAvailable()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    bool down;
    auto vk = kb->getNextVirtualKey(&down, 0);
    if (!down)
      continue;
    if (vk == fabgl::VK_1 || vk == fabgl::VK_KP_1 || vk == fabgl::VK_KP_END)
      return 1;
    if (vk == fabgl::VK_2 || vk == fabgl::VK_KP_2 || vk == fabgl::VK_KP_DOWN)
      return 2;
    if (vk == fabgl::VK_ESCAPE)
      return 0;
  }
}


// Shows the list until a ROM is picked. Returns its index, and in *slot the savestate slot to
// start it from (0: none; for the paused game that means carrying on).
static int chooseRom(char const * message, int * slot)
{
  *slot = 0;
  static int selected = 0;
  const int visible = 20;
  auto kb = PS2.keyboard();
  listRoms();
  s_shown.valid = false;              // a game or something else was on the screen
  if (selected >= g_romCount)
    selected = 0;
  for (;;) {
    int top = std::max(0, std::min(selected - visible / 2, g_romCount - visible));
    drawMenu(selected, top, visible, message);
    // a number on the serial console starts that ROM, for testing without a keyboard
    int n = g_romCount;
    bool down = false;
    fabgl::VirtualKey vk = fabgl::VK_NONE;
    while (vk == fabgl::VK_NONE) {
      // over serial: a number starts that game, m and a number shows its manual
      if (Serial.available()) {
        int c = Serial.peek();
        if (c == 'k' || c == 'f') {
          Serial.read();
          message = c == 'k' ? editPadKeys() : fileManager();
          s_shown.valid = false;
          break;
        }
        if (c == 's' || c == 'l') {
          Serial.read();
          vk = c == 's' ? fabgl::VK_s : fabgl::VK_l;      // as the keys; 'l' takes a game number after the slot
          down = true;
          break;
        }
        if (c != 'm' && c != 'g' && !isdigit(c)) {
          Serial.read();          // stray characters (line ends) would read as game 0
          continue;
        }
        bool manual = c == 'm', codes = c == 'g';
        if (manual || codes)
          Serial.read();
        int idx = Serial.parseInt();
        if (idx >= 0 && idx < n) {
          selected = idx;
          if (codes) {
            editCheats(g_roms[idx]);
            s_shown.valid = false;
            vk = fabgl::VK_NONE;
            break;
          }
          if (!manual)
            return idx;
          if (g_roms[idx].manual) {
            showManual(g_roms[idx]);
            s_shown.valid = false;
          }
          vk = fabgl::VK_NONE;
          break;
        }
      }
      if (kb->virtualKeyAvailable())
        vk = kb->getNextVirtualKey(&down, 0);
      else
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!down)
      continue;
    message = nullptr;
    switch (vk) {
      case fabgl::VK_k: case fabgl::VK_K:
        message = editPadKeys();
        s_shown.valid = false;
        break;
      case fabgl::VK_f: case fabgl::VK_F:
        message = fileManager();
        s_shown.valid = false;
        break;
      case fabgl::VK_g: case fabgl::VK_G:
        if (n) {
          editCheats(g_roms[selected]);
          if (selected == s_paused)
            applyCheats(g_roms[selected]);
          s_shown.valid = false;
        }
        break;
      case fabgl::VK_s: case fabgl::VK_S: {
        if (s_paused < 0) {
          message = "No game paused to save";
          break;
        }
        // the paused game, whatever is selected
        if (selected != s_paused) {
          selected = s_paused;
          top = std::max(0, std::min(selected - visible / 2, g_romCount - visible));
          drawMenu(selected, top, visible, nullptr);
        }
        int slot = askSlot("Save to slot 1 or 2? (Esc)");
        message = slot ? saveSlot(s_paused, slot) : nullptr;
        if (slot)
          drawSlots(s_paused);
        break;
      }
      case fabgl::VK_l: case fabgl::VK_L: {
        if (!n)
          break;
        int s = askSlot("Load from slot 1 or 2? (Esc)");
        if (!s)
          break;
        if (Serial.available()) {             // over serial the game's number follows
          int idx = Serial.parseInt();
          if (idx >= 0 && idx < n)
            selected = idx;
        }
        if (!slotUsed(g_roms[selected], s)) {
          message = s == 1 ? "Slot 1 is empty" : "Slot 2 is empty";
          break;
        }
        *slot = s;
        return selected;
      }
      case fabgl::VK_m: case fabgl::VK_M: case fabgl::VK_TAB:
        if (n && g_roms[selected].manual) {
          showManual(g_roms[selected]);
          s_shown.valid = false;
        } else
          message = "No manual for this game";
        break;
      case fabgl::VK_UP:       case fabgl::VK_KP_UP:       if (n) selected = (selected + n - 1) % n; break;
      case fabgl::VK_DOWN:     case fabgl::VK_KP_DOWN:     if (n) selected = (selected + 1) % n; break;
      case fabgl::VK_PAGEUP:   case fabgl::VK_KP_PAGEUP:   selected = std::max(0, selected - visible); break;
      case fabgl::VK_PAGEDOWN: case fabgl::VK_KP_PAGEDOWN: if (n) selected = std::min(n - 1, selected + visible); break;
      case fabgl::VK_LEFT:     case fabgl::VK_KP_LEFT:
        s_regionChoice = (RegionChoice)(((int)s_regionChoice + 2) % 3);
        break;
      case fabgl::VK_RIGHT:    case fabgl::VK_KP_RIGHT:
        s_regionChoice = (RegionChoice)(((int)s_regionChoice + 1) % 3);
        break;
      case fabgl::VK_RETURN:   case fabgl::VK_KP_ENTER:    case fabgl::VK_SPACE:
        if (n)
          return selected;
        break;
      default:
        break;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// cheat codes: a text file per game next to the saves, one code per line as typed

static void loadCheatCodes(RomBlob const & rom, std::vector<String> & codes)
{
  codes.clear();
  String path = savePathFor(rom, ".cht");
  if (!SPIFFS.exists(path))
    return;
  File f = SPIFFS.open(path, "r");
  while (f && f.available() && (int) codes.size() < Nes::MAX_CHEATS) {
    String line = f.readStringUntil('\n');
    line.trim();
    Cheat c;
    if (line.length() && decodeCheat(line.c_str(), &c))
      codes.push_back(line);
  }
}


static void storeCheatCodes(RomBlob const & rom, std::vector<String> const & codes)
{
  String path = savePathFor(rom, ".cht");
  if (codes.empty()) {
    if (SPIFFS.exists(path))
      SPIFFS.remove(path);
    return;
  }
  s_soundGen->play(false);          // flash writes stop the cache, and the sound interrupt with it
  File f = SPIFFS.open(path, "w");
  for (auto const & c : codes)
    f.print(c + "\n");
  f.close();
  s_soundGen->play(true);
}


// The code editor: type a code and Enter to add it, Up/Down to select, Del to remove the
// selected one, Esc to save and go back. Over serial: characters type, Enter adds, ! removes the
// selected code, ~ leaves.
static void editCheats(RomBlob const & rom)
{
  std::vector<String> codes, before;
  loadCheatCodes(rom, codes);
  before = codes;

  fabgl::Canvas & cv = *s_canvas;
  auto kb = PS2.keyboard();
  String input;
  int selected = 0;
  char const * message = nullptr;
  bool redrawList = true;

  cv.setBrushColor(Color::Black);
  cv.clear();
  cv.setPenColor(Color::BrightYellow);
  String title = rom.title;
  if (title.length() > 24)
    title = title.substring(0, 24);
  cv.drawText(0, 4, "CODES");
  cv.setPenColor(Color::BrightCyan);
  cv.drawText(56, 4, title.c_str());
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 214, "Game Genie, or Action Replay");
  cv.drawText(0, 222, "AAAA:VV. Enter:add Del:remove");
  cv.drawText(0, 231, "Up/Down:select Esc:save+back");

  for (;;) {
    if (redrawList) {
      // the codes with what they decode to, each line padded so no clearing is needed
      for (int i = 0; i < Nes::MAX_CHEATS; ++i) {
        char line[40] = "";
        if (i < (int) codes.size()) {
          Cheat c;
          decodeCheat(codes[i].c_str(), &c);
          char what[20];
          if (c.rom && c.compare >= 0)
            snprintf(what, sizeof(what), "GG %04X=%02X?%02X", c.addr, c.value, c.compare);
          else
            snprintf(what, sizeof(what), "%s %04X=%02X", c.rom ? "GG" : "AR", c.addr, c.value);
          snprintf(line, sizeof(line), "%-12s %s", codes[i].c_str(), what);
        }
        char padded[33];
        snprintf(padded, sizeof(padded), "%-32s", line);
        bool sel = i == selected && i < (int) codes.size();
        cv.setBrushColor(sel ? Color::Blue : Color::Black);
        cv.setPenColor(sel ? Color::BrightWhite : Color::White);
        cv.drawText(0, 20 + i * 9, padded);
      }
      cv.setBrushColor(Color::Black);
      redrawList = false;
    }
    char line[33];
    snprintf(line, sizeof(line), "> %-29s", (input + "_").c_str());
    cv.setPenColor(Color::BrightWhite);
    cv.drawText(0, 178, line);
    snprintf(line, sizeof(line), "%-32s", message ? message : "");
    cv.setPenColor(Color::BrightRed);
    cv.drawText(0, 192, line);
    cv.waitCompletion();

    // one key, from the keyboard or the serial console
    int ch = 0;
    fabgl::VirtualKey vk = fabgl::VK_NONE;
    while (!ch && vk == fabgl::VK_NONE) {
      if (Serial.available()) {
        int c = Serial.read();
        if (c == '~')
          vk = fabgl::VK_ESCAPE;
        else if (c == '!')
          vk = fabgl::VK_DELETE;
        else if (c == '\n' || c == '\r')
          vk = fabgl::VK_RETURN;
        else if (isprint(c))
          ch = c;
        continue;
      }
      if (!kb->virtualKeyAvailable()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      bool down;
      auto k = kb->getNextVirtualKey(&down, 0);
      if (!down)
        continue;
      int a = kb->virtualKeyToASCII(k);
      if (a >= 32 && a < 127 && k != fabgl::VK_DELETE)
        ch = a;
      else
        vk = k;
    }
    message = nullptr;

    if (ch) {
      if ((isalnum(ch) || ch == ':' || ch == '-') && input.length() < 12)
        input += (char) toupper(ch);
      continue;
    }
    switch (vk) {
      case fabgl::VK_ESCAPE:
        if (codes != before)
          storeCheatCodes(rom, codes);
        return;
      case fabgl::VK_BACKSPACE:
        if (input.length())
          input.remove(input.length() - 1);
        break;
      case fabgl::VK_RETURN: case fabgl::VK_KP_ENTER: {
        Cheat c;
        if (!input.length())
          break;
        if (!decodeCheat(input.c_str(), &c)) {
          message = "Not a valid code";
        } else if ((int) codes.size() >= Nes::MAX_CHEATS) {
          message = "The list is full";
        } else {
          char canon[24];
          formatCheat(c, input.c_str(), canon, sizeof(canon));
          codes.push_back(canon);
          selected = codes.size() - 1;
          input = "";
          redrawList = true;
        }
        break;
      }
      case fabgl::VK_DELETE: case fabgl::VK_KP_DELETE:
        if (selected < (int) codes.size()) {
          codes.erase(codes.begin() + selected);
          selected = std::max(0, std::min(selected, (int) codes.size() - 1));
          redrawList = true;
        }
        break;
      case fabgl::VK_UP: case fabgl::VK_KP_UP:
        if (selected > 0) {
          --selected;
          redrawList = true;
        }
        break;
      case fabgl::VK_DOWN: case fabgl::VK_KP_DOWN:
        if (selected + 1 < (int) codes.size()) {
          ++selected;
          redrawList = true;
        }
        break;
      default:
        break;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// the pads: keyboard keys for two players and real NES pads on GPIOs

// Real NES pads (a 4021 shift register in each): latch and clock are shared, each pad has its own
// data line. -1 = not connected. Power the pads from 3.3 V (the 4021 runs on it), so their data
// line is safe for the ESP32; the data GPIOs need a pull-up (internal, so not GPIO 34-39, or an
// external one), so an unplugged pad reads as no buttons.
static constexpr int PAD_LATCH_GPIO   = -1;
static constexpr int PAD_CLOCK_GPIO   = -1;
static constexpr int PAD_DATA_GPIO[2] = { -1, -1 };

static void initPads()
{
  if (PAD_LATCH_GPIO < 0 || PAD_CLOCK_GPIO < 0)
    return;
  pinMode(PAD_LATCH_GPIO, OUTPUT);
  pinMode(PAD_CLOCK_GPIO, OUTPUT);
  digitalWrite(PAD_LATCH_GPIO, LOW);
  digitalWrite(PAD_CLOCK_GPIO, LOW);
  for (int gpio : PAD_DATA_GPIO)
    if (gpio >= 0)
      pinMode(gpio, INPUT_PULLUP);
}

// Reads both pads as the NES does: a latch pulse, then 8 bits, A first, a pressed button is low.
// About 110 us, once per frame.
static void readHardwarePads(uint8_t out[2])
{
  out[0] = out[1] = 0;
  if (PAD_LATCH_GPIO < 0 || PAD_CLOCK_GPIO < 0)
    return;
  digitalWrite(PAD_LATCH_GPIO, HIGH);
  delayMicroseconds(12);
  digitalWrite(PAD_LATCH_GPIO, LOW);
  delayMicroseconds(6);
  for (int bit = 0; bit < 8; ++bit) {
    for (int p = 0; p < 2; ++p)
      if (PAD_DATA_GPIO[p] >= 0 && !digitalRead(PAD_DATA_GPIO[p]))
        out[p] |= 1 << bit;
    digitalWrite(PAD_CLOCK_GPIO, HIGH);
    delayMicroseconds(6);
    digitalWrite(PAD_CLOCK_GPIO, LOW);
    delayMicroseconds(6);
  }
}


// Keyboard keys: per player one key per button, plus the Famicom microphone for player 2. The same
// for all games, kept in SPIFFS (written only on change).
static constexpr char const * KEYS_PATH = "/keys.cfg";
static constexpr int PAD_KEYS = 9;            // the buttons by bit (BTN_A = bit 0 ... BTN_RIGHT = bit 7), then the mic
static constexpr int MIC = 8;

static const fabgl::VirtualKey DEFAULT_PAD_KEYS[2][PAD_KEYS] = {
  { fabgl::VK_LCTRL, fabgl::VK_LALT, fabgl::VK_LSHIFT, fabgl::VK_SPACE,
    fabgl::VK_UP, fabgl::VK_DOWN, fabgl::VK_LEFT, fabgl::VK_RIGHT, fabgl::VK_NONE },
  { fabgl::VK_KP_PERIOD, fabgl::VK_KP_0, fabgl::VK_KP_DIVIDE, fabgl::VK_KP_MULTIPLY,
    fabgl::VK_KP_8, fabgl::VK_KP_5, fabgl::VK_KP_4, fabgl::VK_KP_6, fabgl::VK_KP_PLUS },
};

static fabgl::VirtualKey s_padKeys[2][PAD_KEYS];
static uint8_t s_padState[2];        // the buttons whose keys are down, from the key events
static bool    s_micState;


// One name for the keys that are one key on the keyboard: FabGL gives a letter as a different
// virtual key with Shift held, the numpad depends on Num Lock, and there are left and right
// modifiers. So Shift as Select and a letter as A work together, and a key released after Shift
// still releases its button. (Shifted symbols as on the US layout, FabGL's default.)
static fabgl::VirtualKey canonKey(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  if (vk >= VK_A && vk <= VK_Z)
    return (VirtualKey)(VK_a + (vk - VK_A));
  static const VirtualKey pairs[][2] = {
    { VK_EXCLAIM, VK_1 }, { VK_AT, VK_2 }, { VK_HASH, VK_3 }, { VK_DOLLAR, VK_4 }, { VK_PERCENT, VK_5 },
    { VK_CARET, VK_6 }, { VK_AMPERSAND, VK_7 }, { VK_ASTERISK, VK_8 }, { VK_LEFTPAREN, VK_9 },
    { VK_RIGHTPAREN, VK_0 }, { VK_UNDERSCORE, VK_MINUS }, { VK_PLUS, VK_EQUALS },
    { VK_LEFTBRACE, VK_LEFTBRACKET }, { VK_RIGHTBRACE, VK_RIGHTBRACKET }, { VK_VERTICALBAR, VK_BACKSLASH },
    { VK_COLON, VK_SEMICOLON }, { VK_QUOTEDBL, VK_QUOTE }, { VK_LESS, VK_COMMA }, { VK_GREATER, VK_PERIOD },
    { VK_QUESTION, VK_SLASH }, { VK_TILDE, VK_GRAVEACCENT },
    { VK_RSHIFT, VK_LSHIFT }, { VK_RALT, VK_LALT }, { VK_RCTRL, VK_LCTRL }, { VK_RGUI, VK_LGUI },
    // the numpad is its own keys, with or without Num Lock
    { VK_KP_UP, VK_KP_8 }, { VK_KP_DOWN, VK_KP_2 }, { VK_KP_LEFT, VK_KP_4 }, { VK_KP_RIGHT, VK_KP_6 },
    { VK_KP_HOME, VK_KP_7 }, { VK_KP_END, VK_KP_1 }, { VK_KP_PAGEUP, VK_KP_9 }, { VK_KP_PAGEDOWN, VK_KP_3 },
    { VK_KP_INSERT, VK_KP_0 }, { VK_KP_DELETE, VK_KP_PERIOD }, { VK_KP_CENTER, VK_KP_5 },
  };
  for (auto const & p : pairs)
    if (vk == p[0])
      return p[1];
  return vk;
}


// a key event while a game runs: the pads' buttons follow their keys
static void padKeyEvent(fabgl::VirtualKey vk, bool down)
{
  vk = canonKey(vk);
  for (int p = 0; p < 2; ++p)
    for (int b = 0; b < 8; ++b)
      if (s_padKeys[p][b] == vk)
        s_padState[p] = down ? (s_padState[p] | (1 << b)) : (s_padState[p] & ~(1 << b));
  if (s_padKeys[1][MIC] == vk)
    s_micState = down;
}


// the keys the emulator keeps for itself
static bool reservedKey(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  return vk == VK_ESCAPE || vk == VK_F1 || vk == VK_F2 || vk == VK_F3 || vk == VK_F10;
}


// The file: 2 x 9 key numbers (16 bit), 0 for a button without a key. An older one of 8 numbers
// holds only player 1's buttons.
static void loadPadKeys()
{
  memcpy(s_padKeys, DEFAULT_PAD_KEYS, sizeof(s_padKeys));
  File f = SPIFFS.exists(KEYS_PATH) ? SPIFFS.open(KEYS_PATH, "r") : File();
  uint16_t keys[2 * PAD_KEYS];
  int n = f ? f.read((uint8_t *) keys, sizeof(keys)) / 2 : 0;
  if (n != 8 && n != 2 * PAD_KEYS)
    return;
  for (int i = 0; i < n; ++i)
    if (keys[i] >= fabgl::VK_LAST)
      return;                        // not a file of ours: keep the defaults
  for (int i = 0; i < n; ++i)
    s_padKeys[i / PAD_KEYS][i % PAD_KEYS] = (fabgl::VirtualKey) keys[i];
  // an older file: player 2's default keys that player 1 has stay free
  for (int b = 0; b < PAD_KEYS && n == 8; ++b)
    for (int o = 0; o < 8; ++o)
      if (s_padKeys[1][b] == s_padKeys[0][o])
        s_padKeys[1][b] = fabgl::VK_NONE;
}


static void storePadKeys()
{
  uint16_t keys[2 * PAD_KEYS];
  for (int i = 0; i < 2 * PAD_KEYS; ++i)
    keys[i] = s_padKeys[i / PAD_KEYS][i % PAD_KEYS];
  s_soundGen->play(false);          // flash writes stop the cache, and the sound interrupt with it
  File f = SPIFFS.open(KEYS_PATH, "w");
  if (f) {
    f.write((uint8_t const *) keys, sizeof(keys));
    f.close();
  }
  s_soundGen->play(true);
}


// a short name for a key, at most 11 characters (arrows are the font's code page 437 arrows)
static String keyName(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  switch (vk) {
    case VK_NONE:      return "-";
    case VK_UP:        return "\x18";
    case VK_DOWN:      return "\x19";
    case VK_LEFT:      return "\x1b";
    case VK_RIGHT:     return "\x1a";
    case VK_SPACE:     return "Space";
    case VK_LSHIFT:    return "Shift";
    case VK_LCTRL:     return "Ctrl";
    case VK_LALT:      return "Alt";
    case VK_LGUI:      return "Win";
    case VK_RETURN:    return "Enter";
    case VK_TAB:       return "Tab";
    case VK_BACKSPACE: return "Backspace";
    case VK_INSERT:    return "Insert";
    case VK_DELETE:    return "Delete";
    case VK_HOME:      return "Home";
    case VK_END:       return "End";
    case VK_PAGEUP:    return "Page Up";
    case VK_PAGEDOWN:  return "Page Down";
    case VK_CAPSLOCK:  return "Caps Lock";
    case VK_NUMLOCK:   return "Num Lock";
    case VK_SCROLLLOCK: return "Scroll Lock";
    case VK_APPLICATION: return "Menu";
    case VK_KP_PLUS:   return "Keypad +";
    case VK_KP_MINUS:  return "Keypad -";
    case VK_KP_MULTIPLY: return "Keypad *";
    case VK_KP_DIVIDE: return "Keypad /";
    case VK_KP_PERIOD: return "Keypad .";
    case VK_KP_ENTER:  return "Keypad Ent";
    default:           break;
  }
  if (vk >= VK_F1 && vk <= VK_F12)
    return "F" + String(vk - VK_F1 + 1);
  if (vk >= VK_a && vk <= VK_z)
    return String((char)('A' + (vk - VK_a)));
  if (vk >= VK_KP_0 && vk <= VK_KP_9)
    return "Keypad " + String(vk - VK_KP_0);
  int a = PS2.keyboard()->virtualKeyToASCII(vk);
  if (a > 32 && a < 127)
    return String((char) a);
  return "Key " + String((int) vk);
}


// The pad editor's picture: an NES pad, every button drawn on its own so the one being set can
// blink; player 2's has the Famicom's microphone. Positions in the 256x240 canvas.
static constexpr int PAD_Y  = 30;                 // top of the pad's body
static constexpr int DPAD_X = 66, DPAD_Y = PAD_Y + 54;
static constexpr int AB_Y   = PAD_Y + 56;
static constexpr int MIC_X  = 200, MIC_Y = PAD_Y + 11;

static void drawPadButton(int b, bool lit)
{
  fabgl::Canvas & cv = *s_canvas;
  Color dark = lit ? Color::BrightYellow : Color::BrightBlack;
  if (b == MIC) {
    // a grille: a dark disc with slots
    cv.setBrushColor(dark);
    cv.fillEllipse(MIC_X, MIC_Y, 17, 17);
    cv.setPenColor(Color::White);
    for (int i = -4; i <= 4; i += 4)
      cv.drawLine(MIC_X - 4, MIC_Y + i, MIC_X + 4, MIC_Y + i);
    return;
  }
  switch (1 << b) {
    case BTN_UP:
      cv.setBrushColor(dark);
      cv.fillRectangle(DPAD_X - 6, DPAD_Y - 19, DPAD_X + 6, DPAD_Y - 7);
      break;
    case BTN_DOWN:
      cv.setBrushColor(dark);
      cv.fillRectangle(DPAD_X - 6, DPAD_Y + 7, DPAD_X + 6, DPAD_Y + 19);
      break;
    case BTN_LEFT:
      cv.setBrushColor(dark);
      cv.fillRectangle(DPAD_X - 19, DPAD_Y - 6, DPAD_X - 7, DPAD_Y + 6);
      break;
    case BTN_RIGHT:
      cv.setBrushColor(dark);
      cv.fillRectangle(DPAD_X + 7, DPAD_Y - 6, DPAD_X + 19, DPAD_Y + 6);
      break;
    case BTN_SELECT: case BTN_START: {
      int x = (1 << b) == BTN_SELECT ? 106 : 134;
      cv.setBrushColor(lit ? Color::BrightYellow : Color::Black);
      cv.fillRectangle(x, AB_Y + 2, x + 17, AB_Y + 7);
      break;
    }
    case BTN_B: case BTN_A: {
      int x = (1 << b) == BTN_B ? 178 : 208;
      cv.setBrushColor(lit ? Color::BrightYellow : Color::Red);
      cv.fillEllipse(x, AB_Y, 19, 19);
      break;
    }
  }
}


static void drawPad(int player)
{
  fabgl::Canvas & cv = *s_canvas;
  // the cable and the body, light grey, with the black face
  cv.setBrushColor(Color::BrightBlack);
  cv.fillRectangle(124, PAD_Y - 12, 131, PAD_Y - 1);
  cv.setBrushColor(Color::White);
  cv.fillRectangle(16, PAD_Y, 239, PAD_Y + 95);
  cv.setBrushColor(Color::Black);
  cv.fillRectangle(24, PAD_Y + 22, 231, PAD_Y + 87);
  cv.setPenColor(Color::BrightRed);
  cv.setBrushColor(Color::White);
  cv.drawText(24, PAD_Y + 8, player ? "ESPNES  II" : "ESPNES  I");
  // the D-pad's middle, the arms are buttons
  cv.setBrushColor(Color::BrightBlack);
  cv.fillRectangle(DPAD_X - 6, DPAD_Y - 6, DPAD_X + 6, DPAD_Y + 6);
  // the grey stripes in the middle, the field of Select and Start below them
  cv.setBrushColor(Color::White);
  for (int i = 0; i < 3; ++i)
    cv.fillRectangle(100, PAD_Y + 26 + i * 8, 157, PAD_Y + 29 + i * 8);
  cv.fillRectangle(100, AB_Y - 4, 157, AB_Y + 13);
  // grey squares behind A and B
  cv.fillRectangle(166, AB_Y - 12, 190, AB_Y + 12);
  cv.fillRectangle(196, AB_Y - 12, 220, AB_Y + 12);
  cv.setPenColor(Color::BrightRed);
  cv.setBrushColor(Color::Black);
  cv.drawText(&fabgl::FONT_4x6, 104, AB_Y + 16, "SELECT");
  cv.drawText(&fabgl::FONT_4x6, 134, AB_Y + 16, "START");
  cv.drawText(175, AB_Y + 16, "B");
  cv.drawText(205, AB_Y + 16, "A");
  for (int b = 0; b < (player ? PAD_KEYS : 8); ++b)
    drawPadButton(b, false);
}


// The key mapper. First the player (1 or 2), then a key for each button in turn: the button being
// set blinks on the pad and in the list; a key that another button (of either player) has swaps
// with it. Esc cancels; after the last button the keys are kept. Over serial: 1 or 2 picks the
// player, then a letter, digit or space is that key; ~ cancels, ^ takes the default keys.
static char const * editPadKeys()
{
  static const int order[PAD_KEYS] = { 4, 5, 6, 7, 2, 3, 1, 0, MIC };   // Up Down Left Right Select Start B A Mic
  static char const * const names[PAD_KEYS] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right", "Mic" };
  static constexpr int LIST_TOP = PAD_Y + 104;

  fabgl::VirtualKey keys[2][PAD_KEYS];
  memcpy(keys, s_padKeys, sizeof(keys));
  fabgl::Canvas & cv = *s_canvas;
  auto kb = PS2.keyboard();
  kb->emptyVirtualKeyQueue();

  auto drawMsg = [&](char const * message) {
    char line[33];
    snprintf(line, sizeof(line), "%-32s", message ? message : "");
    cv.setPenColor(Color::BrightRed);
    cv.drawText(0, 212, line);
  };
  // one key, from the keyboard or the serial console; VK_NONE when there is none yet
  auto nextKey = [&]() {
    fabgl::VirtualKey vk = fabgl::VK_NONE;
    if (Serial.available()) {
      int c = tolower(Serial.read());
      if (c == '~')
        vk = fabgl::VK_ESCAPE;
      else if (c == '^')
        vk = fabgl::VK_LAST;         // stands for: the default keys
      else if (c >= 'a' && c <= 'z')
        vk = (fabgl::VirtualKey)(fabgl::VK_a + (c - 'a'));
      else if (c >= '0' && c <= '9')
        vk = (fabgl::VirtualKey)(fabgl::VK_0 + (c - '0'));
      else if (c == ' ')
        vk = fabgl::VK_SPACE;
    } else if (kb->virtualKeyAvailable()) {
      bool down;
      vk = kb->getNextVirtualKey(&down, 0);
      if (!down)
        vk = fabgl::VK_NONE;
    }
    if (vk == fabgl::VK_NONE)
      vTaskDelay(pdMS_TO_TICKS(20));
    return vk;
  };

  cv.setBrushColor(Color::Black);
  cv.clear();
  cv.setPenColor(Color::BrightYellow);
  cv.drawText(0, 4, "KEYS");
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 231, "Esc:cancel");
  drawPad(0);

  // the player
  drawMsg("Which player? 1 or 2");
  cv.waitCompletion();
  int player = -1;
  while (player < 0) {
    auto vk = nextKey();
    if (vk == fabgl::VK_ESCAPE)
      return "Keys not changed";
    if (vk == fabgl::VK_1 || vk == fabgl::VK_KP_1 || vk == fabgl::VK_KP_END)
      player = 0;
    else if (vk == fabgl::VK_2 || vk == fabgl::VK_KP_2 || vk == fabgl::VK_KP_DOWN)
      player = 1;
  }
  int steps = player ? PAD_KEYS : 8;
  fabgl::VirtualKey * mine = keys[player];
  drawMsg(nullptr);
  if (player)
    drawPad(1);
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 222, "Press a key for each button");

  // the list, every line padded to the full width so nothing needs clearing
  auto drawLine = [&](int step, int current, bool cursorOn) {
    int b = order[step];
    char line[33];
    String key = step == current ? (cursorOn ? "_" : " ") : keyName(mine[b]);
    snprintf(line, sizeof(line), "%c %-7s %-22s", step == current ? '>' : ' ', names[b], key.c_str());
    cv.setPenColor(step == current ? Color::BrightYellow : Color::White);
    cv.drawText(0, LIST_TOP + step * 8, line);
  };

  int step = 0;
  bool on = true;
  int64_t nextBlink = 0;
  for (int i = 0; i < steps; ++i)
    drawLine(i, step, on);

  while (step < steps) {
    // blinking: the button on the pad and the cursor in the list
    int64_t now = esp_timer_get_time();
    if (now >= nextBlink) {
      on = !on;
      nextBlink = now + 300000;
      drawPadButton(order[step], on);
      drawLine(step, step, on);
      cv.waitCompletion();
    }

    auto vk = nextKey();
    if (vk == fabgl::VK_NONE)
      continue;
    if (vk == fabgl::VK_ESCAPE)
      return "Keys not changed";
    if (vk == fabgl::VK_LAST) {
      memcpy(mine, DEFAULT_PAD_KEYS[player], sizeof(keys[0]));
      break;
    }
    vk = canonKey(vk);
    if (reservedKey(vk)) {
      drawMsg("Esc F1 F2 F3 F10 are taken");
      continue;
    }
    drawMsg(nullptr);

    // a key another button has: that button gets this one's old key
    int b = order[step];
    for (int p = 0; p < 2; ++p)
      for (int o = 0; o < PAD_KEYS; ++o)
        if ((p != player || o != b) && keys[p][o] == vk) {
          keys[p][o] = mine[b];
          if (p == player)
            for (int i = 0; i < steps; ++i)
              if (order[i] == o)
                drawLine(i, step, on);
        }
    mine[b] = vk;
    drawPadButton(b, false);
    if (++step < steps)
      drawLine(step - 1, step, on);
    on = false;
    nextBlink = 0;                   // the next button lights up at once
  }
  cv.waitCompletion();

  if (!memcmp(keys, s_padKeys, sizeof(keys)))
    return "Keys not changed";
  memcpy(s_padKeys, keys, sizeof(keys));
  storePadKeys();
  for (int p = 0; p < 2; ++p) {
    Serial.printf("Keys player %d:", p + 1);
    for (int b = 0; b < (p ? PAD_KEYS : 8); ++b)
      Serial.printf(" %s=%s", names[b], keyName(s_padKeys[p][b]).c_str());
    Serial.println();
  }
  return "Keys saved";
}


///////////////////////////////////////////////////////////////////////////////
// the file manager: what is in SPIFFS, by game, to delete saves and states of games that are no
// longer built in (their files carry the ROM's CRC, which no game in the list has any more)

struct SpiffsFile {
  String path;
  size_t size;
  int    game;                     // index in g_roms, -1: a removed game, -2: not a game's file
};


static std::vector<SpiffsFile> listSpiffs()
{
  // each game's file name start: "/<stem>_<crc>."
  std::vector<String> prefixes;
  for (int i = 0; i < g_romCount; ++i) {
    String p = savePathFor(g_roms[i], "");
    prefixes.push_back(p + ".");
  }
  std::vector<SpiffsFile> files;
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    SpiffsFile e { String("/") + f.name(), f.size(), -2 };
    if (e.path.startsWith("//"))
      e.path = e.path.substring(1);           // some core versions give the name with its slash
    for (int i = 0; i < g_romCount && e.game == -2; ++i)
      if (e.path.startsWith(prefixes[i]))
        e.game = i;
    // "_" and 8 hex digits before the extension: a game's file, of a game that is gone
    int dot = e.path.lastIndexOf('.');
    if (e.game == -2 && dot >= 10 && e.path[dot - 9] == '_') {
      bool hex = true;
      for (int k = dot - 8; k < dot; ++k)
        hex &= isxdigit((unsigned char) e.path[k]) != 0;
      if (hex)
        e.game = -1;
    }
    files.push_back(e);
  }
  // by game (removed ones last), then by name
  std::sort(files.begin(), files.end(), [](SpiffsFile const & a, SpiffsFile const & b) {
    int ga = a.game < 0 ? INT_MAX - 2 - a.game : a.game, gb = b.game < 0 ? INT_MAX - 2 - b.game : b.game;
    return ga != gb ? ga < gb : a.path < b.path;
  });
  return files;
}


// what a file is, from its extension
static char const * fileKind(String const & path)
{
  if (path == KEYS_PATH)          return "Keys";
  if (path.endsWith(".sav"))      return "Save";
  if (path.endsWith(".cht"))      return "Codes";
  if (path.endsWith(".st1"))      return "State 1";
  if (path.endsWith(".st2"))      return "State 2";
  if (path.endsWith(".stn"))      return "State tmp";
  return "?";
}


// One line of 32 characters: whose, what, how big.
static void fileLine(SpiffsFile const & f, char * line)
{
  String who;
  if (f.game >= 0)
    who = g_roms[f.game].title;
  else if (f.game == -1)
    who = f.path.substring(1, f.path.lastIndexOf('.') - 9);   // the stem of the ROM's name
  else
    who = f.path == KEYS_PATH ? "Settings" : f.path.substring(1);
  if (who.length() > 14)
    who = who.substring(0, 14);
  char size[8];
  if (f.size < 1000)
    snprintf(size, sizeof(size), "%uB", (unsigned) f.size);
  else
    snprintf(size, sizeof(size), "%u.%uK", (unsigned)(f.size / 1024), (unsigned)(f.size % 1024 * 10 / 1024));
  snprintf(line, 33, "%-14s %-9s %6s", who.c_str(), fileKind(f.path), size);
}


// The screen: Up/Down select, Del deletes the selected file, O all files of removed games (both
// ask first), Esc goes back. Over serial: n/p select, d deletes, o removed games, y/n answer, ~ back.
static char const * fileManager()
{
  static constexpr int TOP = 30, ROWS = 18;
  fabgl::Canvas & cv = *s_canvas;
  auto kb = PS2.keyboard();
  std::vector<SpiffsFile> files = listSpiffs();
  int selected = 0, top = 0;
  char const * message = nullptr;
  bool changed = false;

  for (auto const & f : files) {
    char line[33];
    fileLine(f, line);
    Serial.printf("  %s %s%s\n", line, f.path.c_str(), f.game == -1 ? " (removed game)" : "");
  }

  cv.setBrushColor(Color::Black);
  cv.clear();
  cv.setPenColor(Color::BrightYellow);
  cv.drawText(0, 4, "FILES");
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 222, "Del:delete O:of removed games");
  cv.drawText(0, 231, "Esc:back      red: game removed");

  // one key: VK_NONE when there is none yet; serial letters stand for keys
  auto nextKey = [&]() {
    fabgl::VirtualKey vk = fabgl::VK_NONE;
    if (Serial.available()) {
      switch (Serial.read()) {
        case 'n': vk = fabgl::VK_DOWN; break;
        case 'p': vk = fabgl::VK_UP; break;
        case 'd': vk = fabgl::VK_DELETE; break;
        case 'o': vk = fabgl::VK_o; break;
        case 'y': vk = fabgl::VK_y; break;
        case '~': vk = fabgl::VK_ESCAPE; break;
        default:  vk = fabgl::VK_n; break;   // anything else answers no
      }
    } else if (kb->virtualKeyAvailable()) {
      bool down;
      vk = kb->getNextVirtualKey(&down, 0);
      if (!down)
        vk = fabgl::VK_NONE;
    }
    if (vk == fabgl::VK_NONE)
      vTaskDelay(pdMS_TO_TICKS(20));
    return vk;
  };
  auto drawMsg = [&](char const * text) {
    char line[33];
    snprintf(line, sizeof(line), "%-32s", text ? text : "");
    if (text)
      Serial.printf("Files: %s\n", text);
    cv.setPenColor(Color::BrightRed);
    cv.drawText(0, 208, line);
  };
  auto confirm = [&](char const * question) {
    drawMsg(question);
    cv.waitCompletion();
    for (;;) {
      auto vk = nextKey();
      if (vk == fabgl::VK_y || vk == fabgl::VK_Y)
        return true;
      if (vk != fabgl::VK_NONE)
        return false;
    }
  };
  auto remove = [&](SpiffsFile const & f) {
    s_soundGen->play(false);        // flash writes stop the cache, and the sound interrupt with it
    bool ok = SPIFFS.remove(f.path);
    s_soundGen->play(true);
    Serial.printf("Removed %s: %s\n", f.path.c_str(), ok ? "ok" : "FAILED");
    if (ok && f.path == KEYS_PATH)
      loadPadKeys();                // back to the default keys
    changed |= ok;
    return ok;
  };

  for (;;) {
    // the usage, then the list (every line padded, so nothing needs clearing)
    char line[33];
    snprintf(line, sizeof(line), "%u of %u KB used", (unsigned)(SPIFFS.usedBytes() / 1024), (unsigned)(SPIFFS.totalBytes() / 1024));
    snprintf(line, sizeof(line), "%-26s", String(line).c_str());
    cv.setPenColor(Color::White);
    cv.drawText(48, 4, line);
    selected = std::max(0, std::min(selected, (int) files.size() - 1));
    top = std::max(0, std::min(top, selected));
    if (selected >= top + ROWS)
      top = selected - ROWS + 1;
    for (int r = 0; r < ROWS; ++r) {
      int i = top + r;
      bool sel = i == selected;
      if (i < (int) files.size())
        fileLine(files[i], line);
      else
        snprintf(line, sizeof(line), "%-32s", i == 0 ? "(no files)" : "");
      cv.setBrushColor(sel && i < (int) files.size() ? Color::Blue : Color::Black);
      cv.setPenColor(i < (int) files.size() && files[i].game == -1 ? Color::BrightRed :
                     sel ? Color::BrightWhite : Color::White);
      cv.drawText(0, TOP + r * 9, line);
    }
    cv.setBrushColor(Color::Black);
    drawMsg(message);
    cv.waitCompletion();
    message = nullptr;

    fabgl::VirtualKey vk;
    while ((vk = nextKey()) == fabgl::VK_NONE)
      ;
    switch (vk) {
      case fabgl::VK_ESCAPE:
        return changed ? "Files deleted" : nullptr;
      case fabgl::VK_UP: case fabgl::VK_KP_UP:
        --selected;
        break;
      case fabgl::VK_DOWN: case fabgl::VK_KP_DOWN:
        ++selected;
        break;
      case fabgl::VK_PAGEUP: case fabgl::VK_KP_PAGEUP:
        selected -= ROWS;
        break;
      case fabgl::VK_PAGEDOWN: case fabgl::VK_KP_PAGEDOWN:
        selected += ROWS;
        break;
      case fabgl::VK_DELETE: case fabgl::VK_KP_DELETE: case fabgl::VK_BACKSPACE: {
        if (files.empty())
          break;
        SpiffsFile const & f = files[selected];
        char question[33];
        snprintf(question, sizeof(question), "Delete %s? Y/N", f.game >= 0 || f.game == -1 ? fileKind(f.path) : "it");
        if (confirm(question)) {
          if (remove(f)) {
            files.erase(files.begin() + selected);
            message = "Deleted";
          } else
            message = "Could not delete it";
        }
        break;
      }
      case fabgl::VK_o: case fabgl::VK_O: {
        int n = std::count_if(files.begin(), files.end(), [](SpiffsFile const & f) { return f.game == -1; });
        if (!n) {
          message = "No files of removed games";
          break;
        }
        char question[33];
        snprintf(question, sizeof(question), "Delete %d file%s of removed? Y/N", n, n == 1 ? "" : "s");
        if (confirm(question)) {
          for (int i = files.size() - 1; i >= 0; --i)
            if (files[i].game == -1 && remove(files[i]))
              files.erase(files.begin() + i);
          message = "Deleted";
        }
        break;
      }
      default:
        break;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// a short message over the game picture ("CHEATS ON"), drawn into each line by the core that
// draws them, so it does not flicker

static char         s_overlayText[24];
static volatile int s_overlayFrames;

static void overlayHook(void *, int y, uint8_t * line)
{
  if (!s_overlayFrames || y < 8 || y >= 18)
    return;
  int len = strlen(s_overlayText);
  uint8_t fg = s_raw[0x30], bg = s_raw[0x0f];
  int row = y - 9;                              // a black box from line 8 to 17, text in 9-16
  for (int x = 6; x < 10 + len * 8 && x < 256; ++x) {
    uint8_t px = bg;
    if (row >= 0 && row < 8 && x >= 8 && x < 8 + len * 8) {
      uint8_t ch = s_overlayText[(x - 8) >> 3];
      uint8_t bits = fabgl::FONT_8x8.data[ch * 8 + row];
      if (bits & (0x80 >> ((x - 8) & 7)))
        px = fg;
    }
    line[x ^ 2] = px;                           // the DMA's byte order
  }
}


static void showOverlay(char const * text)
{
  strncpy(s_overlayText, text, sizeof(s_overlayText) - 1);
  s_overlayFrames = 90;
}


///////////////////////////////////////////////////////////////////////////////
// running a game

// Per game settings, from the options column of roms/settings.csv and an optional <rom>.cfg
// (merged by tools/embed_roms.py):
//   save      = yes | no      battery backed save RAM, overriding the header's battery flag
//   region    = ntsc | pal    TV system, overriding the header and the file name
//   overclock = 0 - 100       percent more CPU time per frame, for games that slow down
// One "key = value" per line; '#' starts a comment.
struct RomConfig {
  int save      = -1;       // -1 = from the header
  int region    = -1;       // -1 = automatic, else (int) Region
  int overclock = 0;
};

static RomConfig parseConfig(char const * text)
{
  RomConfig cfg;
  if (!text)
    return cfg;
  while (*text) {
    char const * end = strchr(text, '\n');
    String line = end ? String(text).substring(0, end - text) : String(text);
    text = end ? end + 1 : text + strlen(text);
    int hash = line.indexOf('#');
    if (hash >= 0)
      line = line.substring(0, hash);
    int eq = line.indexOf('=');
    if (eq < 0)
      continue;
    String key = line.substring(0, eq), value = line.substring(eq + 1);
    key.trim();
    value.trim();
    key.toLowerCase();
    value.toLowerCase();
    bool yes = value == "yes" || value == "on" || value == "true" || value == "1";
    bool no  = value == "no" || value == "off" || value == "false" || value == "0";
    if (key == "save" && (yes || no))
      cfg.save = yes;
    else if (key == "region" && (value == "ntsc" || value == "pal"))
      cfg.region = (int)(value == "pal" ? Region::PAL : Region::NTSC);
    else if (key == "overclock" && value.length() && isdigit((unsigned char) value[0]))
      cfg.overclock = std::min((long) Nes::MAX_OVERCLOCK, value.toInt());
    else
      Serial.printf("config: ignoring \"%s\"\n", line.c_str());
  }
  return cfg;
}


// the menu's choice beats the .cfg, which beats the header, which beats the file name
static Region chooseRegion(String const & name, RomConfig const & cfg)
{
  if (s_regionChoice == RegionChoice::NTSC)
    return Region::NTSC;
  if (s_regionChoice == RegionChoice::PAL)
    return Region::PAL;
  if (cfg.region >= 0)
    return (Region) cfg.region;
  if (nes.cart().info.regionKnown)
    return nes.cart().info.region;
  // GoodNES / No-Intro naming
  for (char const * tag : { "(E)", "(Europe)", "(PAL)", "(EU)", "(G)", "(Germany)" })
    if (name.indexOf(tag) >= 0)
      return Region::PAL;
  return Region::NTSC;
}


///////////////////////////////////////////////////////////////////////////////
// save RAM: loaded when a game starts, written back when leaving it, only if it changed

static String   s_savePath;          // empty: the game has no battery backed RAM
static uint32_t s_saveHash;          // of the save RAM as loaded


static uint32_t hashSaveRam()
{
  uint32_t h = 2166136261u;          // FNV-1a
  for (int i = 0; i < Nes::PRG_RAM_SIZE; ++i)
    h = (h ^ nes.prgRam[i]) * 16777619u;
  return h;
}


// The CRC32 of a game's ROM, worked out once (a few ms per 100 KB)
static uint32_t romCrc(RomBlob const & rom)
{
  static std::vector<uint32_t> crcs;           // 0: not yet
  int idx = &rom - g_roms;
  if (crcs.empty())
    crcs.resize(g_romCount);
  if (!crcs[idx])
    crcs[idx] = crc32_le(0, rom.data, rom.size);
  return crcs[idx];
}


// SPIFFS names are at most 31 characters: a readable start of the ROM's name, then the CRC32 of
// its contents, so a renamed ROM keeps its save
static String savePathFor(RomBlob const & rom, char const * ext)
{
  String stem;
  for (char const * p = rom.name; *p && stem.length() < 14; ++p) {
    char c = *p;
    if (c == '.' && !strcasecmp(p, ".nes"))
      break;
    stem += isalnum((unsigned char) c) ? c : '_';
  }
  char crc[10];
  snprintf(crc, sizeof(crc), "_%08x", (unsigned) romCrc(rom));
  return "/" + stem + crc + ext;
}


static void loadSave()
{
  File f = SPIFFS.exists(s_savePath) ? SPIFFS.open(s_savePath, "r") : File();
  if (f) {
    size_t got = f.read(nes.prgRam, Nes::PRG_RAM_SIZE);
    f.close();
    Serial.printf("Save RAM loaded from %s (%u bytes)\n", s_savePath.c_str(), (unsigned) got);
  } else {
    Serial.printf("No save yet (%s)\n", s_savePath.c_str());
  }
  s_saveHash = hashSaveRam();
}


static void storeSave()
{
  if (s_savePath.isEmpty())
    return;
  uint32_t h = hashSaveRam();
  if (h == s_saveHash) {
    Serial.println("Save RAM unchanged, not written");
    return;
  }
  // writing flash stops the cache, and with it the sound interrupt: pause the sound around it
  s_soundGen->play(false);
  File f = SPIFFS.open(s_savePath, "w");
  size_t written = f ? f.write(nes.prgRam, Nes::PRG_RAM_SIZE) : 0;
  if (f)
    f.close();
  s_soundGen->play(true);
  if (written == Nes::PRG_RAM_SIZE) {
    s_saveHash = h;
    Serial.printf("Save RAM written to %s\n", s_savePath.c_str());
  } else {
    Serial.printf("Writing %s FAILED\n", s_savePath.c_str());
  }
}


// the game's codes into the console, all on
static void applyCheats(RomBlob const & rom)
{
  std::vector<String> codes;
  loadCheatCodes(rom, codes);
  Cheat cheats[Nes::MAX_CHEATS];
  int cheatCount = 0;
  for (auto const & c : codes)
    if (decodeCheat(c.c_str(), &cheats[cheatCount]))
      ++cheatCount;
  nes.setCheats(cheats, cheatCount);
  nes.enableCheats(cheatCount > 0);
  if (cheatCount)
    Serial.printf("%d cheat code(s) on\n", cheatCount);
}


///////////////////////////////////////////////////////////////////////////////
// savestates: two slots per game in SPIFFS, compressed (see src/nes/state.h), 1-12 KB each

// SPIFFS space a save leaves free, so the file system keeps room to work (garbage collection,
// wear levelling) and the battery saves always fit
static constexpr size_t SPIFFS_RESERVE = 48 * 1024;

static String slotPath(RomBlob const & rom, int slot)
{
  return savePathFor(rom, slot == 1 ? ".st1" : ".st2");
}


static bool slotUsed(RomBlob const & rom, int slot)
{
  return SPIFFS.exists(slotPath(rom, slot));
}


struct FileSink : StateSink {
  File & f;
  size_t written = 0;
  explicit FileSink(File & file) : f(file) { }
  bool write(uint8_t const * data, size_t size) override
  {
    written += size;
    return f.write(data, size) == size;
  }
};

struct FileSource : StateSource {
  File & f;
  explicit FileSource(File & file) : f(file) { }
  size_t read(uint8_t * data, size_t size) override  { return f.read(data, size); }
};


// Saves the paused game's state. Refuses when SPIFFS would get too full; writes a new file first
// and only then replaces the slot, so a failed save leaves the old state. Returns a message.
static char const * saveSlot(int idx, int slot)
{
  RomBlob const & rom = g_roms[idx];
  String path = slotPath(rom, slot), temp = savePathFor(rom, ".stn");
  size_t old = 0;
  if (SPIFFS.exists(path)) {
    File f = SPIFFS.open(path, "r");
    old = f.size();
  }
  size_t free = SPIFFS.totalBytes() - SPIFFS.usedBytes() + old;
  if (free < nes.stateSizeLimit() + SPIFFS_RESERVE)
    return "Not enough space for a state";

  uint32_t t0 = millis();
  s_soundGen->play(false);          // flash writes stop the cache, and the sound interrupt with it
  File f = SPIFFS.open(temp, "w");
  FileSink sink(f);
  bool ok = f && nes.saveState(sink);
  size_t size = sink.written;
  if (f)
    f.close();
  if (ok) {
    if (old || SPIFFS.exists(path))
      SPIFFS.remove(path);
    ok = SPIFFS.rename(temp, path);
  } else {
    SPIFFS.remove(temp);
  }
  s_soundGen->play(true);
  Serial.printf("State %s %s: %u bytes, %u ms, SPIFFS %u of %u bytes used\n", path.c_str(), ok ? "saved" : "NOT saved",
                (unsigned) size, (unsigned)(millis() - t0), (unsigned) SPIFFS.usedBytes(), (unsigned) SPIFFS.totalBytes());
  if (!ok)
    return "Saving the state failed";
  return slot == 1 ? "Saved in slot 1" : "Saved in slot 2";
}


// Into the freshly loaded game. Returns nullptr or a message.
static char const * loadSlot(RomBlob const & rom, int slot)
{
  File f = SPIFFS.open(slotPath(rom, slot), "r");
  if (!f)
    return "The slot is empty";
  FileSource source(f);
  char const * err = nes.loadState(source);
  Serial.printf("State %s: %s\n", slotPath(rom, slot).c_str(), err ? err : "loaded");
  return err;
}


// Returns nullptr or an error message.
static char const * loadRom(RomBlob const & rom)
{
  // the image stays in flash: the cartridge reads it in place
  if (char const * err = nes.insert(rom.data, rom.size))
    return err;
  RomConfig cfg = parseConfig(rom.config);
  nes.setRegion(chooseRegion(rom.name, cfg));

  bool battery = cfg.save >= 0 ? cfg.save : nes.cart().info.battery;
  s_savePath = battery ? savePathFor(rom) : String();
  if (battery)
    loadSave();

  nes.setOverclock(cfg.overclock);
  applyCheats(rom);
  nes.powerOn();
  Serial.printf("Loaded %s: mapper %d, PRG %d KB, CHR %d KB%s, %s%s, overclock %d%%, free heap %u\n",
                rom.name, nes.cart().info.mapper, nes.cart().info.prgSize / 1024,
                nes.cart().info.chrSize / 1024, nes.cart().info.chrSize ? "" : " (RAM)",
                nes.region() == Region::PAL ? "PAL" : "NTSC", battery ? ", battery save" : "",
                nes.overclock(), (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
  return nullptr;
}


static uint8_t filterPad(uint8_t b)
{
  // a real pad cannot press opposite directions; some games crash when it happens
  if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN))       b &= ~(BTN_UP | BTN_DOWN);
  if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= ~(BTN_LEFT | BTN_RIGHT);
  return b;
}


// the buttons of both players, keyboard and real pads together
static void updatePads(uint8_t serialButtons)
{
  uint8_t hw[2];
  readHardwarePads(hw);
  for (int p = 0; p < 2; ++p)
    nes.setButtons(p, filterPad(s_padState[p] | hw[p] | (p ? 0 : serialButtons)));
  nes.setMic(s_micState);
}


static void toggleCheats()
{
  if (!nes.cheatCount()) {
    showOverlay("NO CODES");
    return;
  }
  nes.enableCheats(!nes.cheatsEnabled());
  showOverlay(nes.cheatsEnabled() ? "CHEATS ON" : "CHEATS OFF");
  Serial.println(nes.cheatsEnabled() ? "Cheats on" : "Cheats off");
}


// Runs the loaded game until Esc.
static void runGame()
{
  auto kb = PS2.keyboard();
  kb->emptyVirtualKeyQueue();
  s_padState[0] = s_padState[1] = 0;
  s_micState = false;

  // NTSC 60.0988 Hz, PAL 50.007 Hz
  const int64_t period = nes.region() == Region::PAL ? 19997 : 16639;
  const int maxSkip = 3;

  // the DAC's real rate (the I2S clock divider cannot hit every rate exactly)
  const int audioRate = s_apuStream.sampleRate() ? s_apuStream.sampleRate() : AUDIO_RATE;
  const uint32_t audioTarget = audioRate * 3 / nes.frameRate();
  nes.apu.setSampleRate(audioRate);
  uint32_t minFill = RING_SIZE;

  uint8_t serialButtons = 0;
  int serialFrames = 0;

  int64_t next = esp_timer_get_time();
  int skipped = 0;
  bool draw = true;

  // statistics, printed every 5 seconds
  int64_t statStart = next, busy = 0, worst = 0;
  int frames = 0, drawn = 0;
  uint64_t waitCycles = 0, drawCycles = 0;

  for (;;) {
    // keys other than the pad
    while (kb->virtualKeyAvailable()) {
      bool down;
      auto vk = kb->getNextVirtualKey(&down, 0);
      padKeyEvent(vk, down);
      if (!down)
        continue;
      if (vk == fabgl::VK_ESCAPE)
        return;
      if (vk == fabgl::VK_F1)
        nes.powerOn();
      if (vk == fabgl::VK_F10)
        toggleCheats();
      if (vk == fabgl::VK_F2 || vk == fabgl::VK_F3) {
        s_volume = std::max(0, std::min(8, s_volume + (vk == fabgl::VK_F3 ? 1 : -1)));
        Serial.printf("Volume %d/8\n", s_volume);
      }
    }
    // serial console, for testing without a keyboard: 'q' leaves, a b e(select) s(tart) u d l r
    // tap a button for 10 frames
    if (Serial.available()) {
      int c = Serial.read();
      if (c == 'q')
        return;
      if (c == 'c')
        toggleCheats();
      static char const keys[] = "abesudlr";
      if (char const * k = strchr(keys, c)) {
        serialButtons = 1 << (k - keys);
        serialFrames = 10;
      }
    }
    updatePads(serialFrames ? serialButtons : 0);
    if (serialFrames)
      --serialFrames;

    int64_t t0 = esp_timer_get_time();
    if (s_overlayFrames)
      --s_overlayFrames;
    nes.runFrame(draw);
    int64_t t1 = esp_timer_get_time();
    if (draw) {
      waitCycles += nes.profWait;
      drawCycles += nes.profDraw;
    }
    busy += t1 - t0;
    worst = std::max(worst, t1 - t0);
    ++frames;
    drawn += draw;

    // keep about three frames of sound buffered, by producing slightly more or fewer samples
    pushSamples();
    int32_t error = (int32_t) audioTarget - (int32_t) ringFill();
    float adjust = 1.0f + 0.005f * std::max(-1.0f, std::min(1.0f, (float) error / audioTarget));
    nes.apu.setSampleRate(audioRate, adjust);
    minFill = std::min(minFill, ringFill());

    if (nes.cpu.jammed) {
      Serial.printf("CPU jammed at $%04X\n", nes.cpu.PC);
      return;
    }

    // pacing: wait for the frame's time slot; when behind, skip drawing to catch up
    next += period;
    int64_t now = esp_timer_get_time();
    if (now < next) {
      int64_t wait = next - now;
      // sleep in whole ticks (1 ms), which can overshoot by one; busy wait the rest
      if (wait > 3000)
        vTaskDelay(pdMS_TO_TICKS((wait - 2000) / 1000));
      while (esp_timer_get_time() < next)
        ;
      draw = true;
      skipped = 0;
    } else {
      if (now - next > period * 4)
        next = now;                   // hopelessly behind: stop trying to catch up
      draw = skipped >= maxSkip;
      skipped = draw ? 0 : skipped + 1;
    }

    if (now - statStart >= 5000000) {
      Serial.printf("%.1f fps (%.1f drawn), emulation %lld us/frame avg, %lld worst (budget %lld)\n",
                    frames * 1e6 / (now - statStart), drawn * 1e6 / (now - statStart),
                    busy / frames, worst, period);
      Serial.printf("  sound buffer %u samples, lowest %u (target %u)\n", (unsigned) ringFill(), (unsigned) minFill, (unsigned) audioTarget);
      minFill = RING_SIZE;
      if (drawn)
        Serial.printf("  core 0 drawing %llu us/frame, core 1 waiting for it %llu us/frame\n",
                      drawCycles / drawn / 240, waitCycles / drawn / 240);
      waitCycles = drawCycles = 0;
      statStart = now;
      busy = worst = 0;
      frames = drawn = 0;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////

void setup()
{
  // A buffered TX, so the statistics line does not block the emulation for 13 ms and make it
  // skip a frame every time it is printed
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESPNES");

  PS2.begin(PS2Preset::KeyboardPort0, KbdMode::CreateVirtualKeysQueue);
  initVideo();
  initSound();
  s_canvas = new fabgl::Canvas(&VGA);
  s_canvas->selectFont(&fabgl::FONT_8x8);
  s_canvas->setGlyphOptions(GlyphOptions().FillBackground(true));

  char const * message = nullptr;
  // for save games later; an unformatted partition is formatted on the first boot
  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS could not be mounted");
  loadPadKeys();
  initPads();

  Serial.printf("Free heap: %u bytes, largest block %u\n",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  for (;;) {
    int slot;
    int idx = chooseRom(message, &slot);
    message = nullptr;
    if (idx != s_paused || slot) {
      // a new game: the paused one ends
      s_paused = -1;
      message = loadRom(g_roms[idx]);
      if (!message && slot)
        message = loadSlot(g_roms[idx], slot);
      if (message)
        continue;
    }
    s_canvas->waitCompletion();
    runGame();
    storeSave();
    // the console waits as it is, unless the game crashed it
    s_paused = nes.cpu.jammed ? -1 : idx;
    if (s_paused >= 0)
      message = "Paused: Enter goes on, S saves";
  }
}


void loop()
{
}
