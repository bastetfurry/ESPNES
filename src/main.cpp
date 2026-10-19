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
//       mic on the numpad (all changed in the main menu's options); real NES pads on GPIOs (see
//       PAD_LATCH_GPIO). SysRq (Alt+PrintScreen) or A+B+Select+Start = the system menu
//       (savestates, tape, cheats, volume, reset, back to the main menu) in every game; F1 = reset, F2/F3 = volume down/up, F10 = cheats on/off,
//       F5/F6/F7 = the Data Recorder's play/record/stop (see "Data Recorder"). Esc is the game's.
//       Family BASIC: the keyboard is the Famicom's, Scroll Lock switches it to the pads and back.
// The main menu (see "the main menu") works with the pad's buttons; Start opens its options
// (manual, savestates, cheat codes, TV system, keys, files, about). "Main Menu" in the system menu pauses
// the game; A on the paused game carries on.
// Serial console: a number starts that ROM from the list, 'q' leaves the game, Z (or Ctrl+Z) sends a screenshot,
// and a b e s u d l r tap A, B, sElect, Start and the directions (in the menus too); P R X play, record and stop the tape,
// U loads one (tools/tape.py), y opens the system menu, T and a line types it on the Famicom keyboard.

#include "compat.h"
#include <vector>
#include <algorithm>
#include <atomic>

#include "fabgl.h"
#include "esp_timer.h"
#include "esp32/rom/miniz.h"

#include "nes/nes.h"
#include "nes/palette.h"
#include "nes/nsf.h"
#include "romindex.h"
#include "fonts.h"
#include "ui.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"


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
  // the menus' colours (ui::Colour)
  static const RGB222 uiColours[ui::COLOURS] = {
    { 0, 0, 0 }, { 0, 0, 3 }, { 3, 3, 3 }, { 1, 1, 1 }, { 0, 0, 2 }, { 3, 3, 0 },
    { 3, 0, 0 }, { 0, 3, 0 }, { 3, 3, 0 }, { 3, 3, 3 }, { 0, 3, 3 }, { 3, 0, 3 },
  };
  uint8_t uiRaw[ui::COLOURS];
  for (int i = 0; i < ui::COLOURS; ++i)
    uiRaw[i] = VGA.createRawPixel(uiColours[i]);
  ui::init(s_lines, uiRaw);
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
  // when the buffer runs dry, hold the last level: that is silent, a jump to 0 would click;
  // the menus' sounds on top
  int v = s_lastSample + (ui::soundSample() * s_volume >> 2);
  return v > 127 ? 127 : v < -127 ? -127 : v;
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
  ui::soundRate(s_apuStream.sampleRate() ? s_apuStream.sampleRate() : AUDIO_RATE);
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
struct CheatEntry {
  bool   on;
  String code, comment;
};
static void   loadCheats(RomBlob const & rom, std::vector<CheatEntry> & list);
static char const * editPadKeys();
static char const * fileManager();
static bool   slotUsed(RomBlob const & rom, int slot);
static char const * saveSlot(int idx, int slot);
static void   applyCheats(RomBlob const & rom);
static void   showOverlay(char const * text);
static void   sendScreenshot();

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
// masked: only where the menu's grid shows the picture (not under a popup)
static void drawPicture(int idx, int left, int top, bool masked = false)
{
  auto * u = (PictureUnpack *) malloc(sizeof(PictureUnpack));
  uint8_t const * pic = u && unpackPicture(g_roms[idx], u) ? u->pixels : nullptr;
  for (int y = 0; y < PICTURE_SIZE; ++y) {
    uint8_t * line = s_lines[top + y];
    for (int x = 0; x < PICTURE_SIZE; ++x)   // the DMA's byte order
      if (!masked || ui::pictureShown(left + x, top + y))
        line[(left + x) ^ 2] = s_raw[pic ? pic[y * PICTURE_SIZE + x] & 0x3f : 0x0f];
  }
  free(u);
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


// Keyboard layouts: FabGL's (US, UK, German, Italian, Spanish, French, Belgian, Norwegian,
// Japanese), chosen in the system menu and kept in SPIFFS (/layout.cfg, its short name). Text,
// menu keys and key names follow the layout. The pads' keys are kept by their place on the
// keyboard: the key the US layout has there (a "position"), so they stay when the layout changes.
static constexpr char const * LAYOUT_PATH = "/layout.cfg";
static int s_layout = -1;            // in fabgl::SupportedLayouts, -1 until loaded

static fabgl::KeyboardLayout const * currentLayout()
{
  return PS2.keyboard()->getLayout();
}

// a layout's key for a scancode (set 2, without E0/F0), following the layouts it builds on
static fabgl::VirtualKey layoutVK(fabgl::KeyboardLayout const * layout, uint8_t code, bool extended)
{
  for (; layout; layout = layout->inherited) {
    auto const * def = extended ? layout->exScancodeToVK : layout->scancodeToVK;
    for (; def->scancode; ++def)
      if (def->scancode == code)
        return def->virtualKey;
  }
  return fabgl::VK_NONE;
}

// the place of a key event on the keyboard: the US layout's key there
static fabgl::VirtualKey positionKey(fabgl::VirtualKeyItem const & item)
{
  uint8_t const * code = item.scancode;
  if (code[0] == 0xe1)
    return fabgl::VK_PAUSE;
  bool extended = code[0] == 0xe0;
  if (extended)
    ++code;
  if (*code == 0xf0)
    ++code;
  fabgl::VirtualKey vk = layoutVK(&fabgl::USLayout, *code, extended);
  return vk != fabgl::VK_NONE ? vk : item.vk;
}

// the current layout's key at a position (for its name)
static fabgl::VirtualKey layoutKey(fabgl::VirtualKey position)
{
  for (int extended = 0; extended < 2; ++extended) {
    auto const * def = extended ? fabgl::USLayout.exScancodeToVK : fabgl::USLayout.scancodeToVK;
    for (; def->scancode; ++def)
      if (def->virtualKey == position) {
        fabgl::VirtualKey vk = layoutVK(currentLayout(), def->scancode, extended);
        return vk != fabgl::VK_NONE ? vk : position;
      }
  }
  return position;
}

static void setLayout(int index)
{
  int n = fabgl::SupportedLayouts::count();
  s_layout = (index % n + n) % n;
  PS2.keyboard()->setLayout(fabgl::SupportedLayouts::layouts()[s_layout]);
}

static void loadLayout()
{
  int index = -1;
  if (File f = SPIFFS.open(LAYOUT_PATH, "r")) {
    String name = f.readStringUntil('\n');
    name.trim();
    for (int i = 0; i < fabgl::SupportedLayouts::count(); ++i)
      if (name == fabgl::SupportedLayouts::shortNames()[i])
        index = i;
  }
  for (int i = 0; index < 0 && i < fabgl::SupportedLayouts::count(); ++i)
    if (!strcmp(fabgl::SupportedLayouts::shortNames()[i], "US"))
      index = i;
  setLayout(index);
  Serial.printf("Keyboard layout: %s\n", currentLayout()->desc);
}

static void storeLayout()
{
  File f = SPIFFS.open(LAYOUT_PATH, "w");
  if (f)
    f.print(String(currentLayout()->name) + "\n");
  Serial.printf("Keyboard layout: %s%s\n", currentLayout()->desc, f ? "" : " (not saved)");
}


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


///////////////////////////////////////////////////////////////////////////////
// Family BASIC keyboard (see nes/famikey.h)
//
// For games that want it (keyboard = yes, or "BASIC" in the file name), the PC keyboard is the
// Famicom's. Letters, digits and symbols type what the layout prints on them (Shift is made what
// the Famicom needs for that character: Shift+, on a German keyboard is ";", which the Famicom has
// unshifted); a character the Famicom does not have (an umlaut) is the Famicom key at that place.
// The other keys go by place. Only SysRq (the system menu) and Scroll Lock (the keyboard as the
// pads and back) stay the emulator's.
//   Famicom      PC
//   ESC          Esc, Tab           CTR    Ctrl         GRPH   Alt (left)   KANA   Caps Lock
//   STOP         Pause/Break        DEL    Backspace, Delete                INS    Insert
//   CLR HOME     Home               YEN    the key left of 1 (by place)     _      Menu key
// AltGr is left to the layout (@ \\ [ ] on QWERTZ and AZERTY).

static bool    s_famiMode;          // the keyboard is the Famicom's (else it drives the pads)
static int     s_famiStopFrames;    // Pause sends no release: STOP is held for a few frames
static String  s_famiTyping;        // text typed over serial ('T'), a key every 8 frames
static int     s_famiTypingFrame;

// the keys held, by position: which Famicom key each pressed, and the Shift it wants
// (-1: the real Shift, 0: none, 1: Shift). Family BASIC loses a key that goes down in the same
// frame as Shift changes (and the ones after it), so a key that changes Shift waits 2 frames;
// released before that, it is still typed, held for 3 frames.
struct FamiHeld {
  fabgl::VirtualKey position;
  FamiKey key;
  int8_t  shift;
  int8_t  wait;                     // frames until it goes down
  bool    released;                 // let go while waiting
  int8_t  hold;                     // then: frames until it goes up
};
static FamiHeld s_famiHeld[12];
static int      s_famiHeldCount;
static bool     s_famiShift;        // the Famicom's Shift as set last

// the Famicom key at a position (the US layout's key there), for keys that go by place
static FamiKey famiKeyAt(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  // before canonKey, which makes the right modifiers the left ones
  if (vk == VK_RSHIFT)
    return FK_RSHIFT;
  if (vk == VK_RALT)
    return FK_NONE;                 // AltGr: the layout's
  vk = canonKey(vk);
  bool shift;
  if (vk >= VK_a && vk <= VK_z)
    return famiKeyOfChar('a' + (vk - VK_a), &shift);
  if (vk >= VK_0 && vk <= VK_9)
    return famiKeyOfChar('0' + (vk - VK_0), &shift);
  if (vk >= VK_F1 && vk <= VK_F8) {
    static const FamiKey f[8] = { FK_F1, FK_F2, FK_F3, FK_F4, FK_F5, FK_F6, FK_F7, FK_F8 };
    return f[vk - VK_F1];
  }
  switch (vk) {
    case VK_ESCAPE: case VK_TAB:         return FK_ESC;
    case VK_LCTRL:                       return FK_CTR;
    case VK_LALT:                        return FK_GRPH;
    case VK_LSHIFT:                      return FK_LSHIFT;
    case VK_CAPSLOCK:                    return FK_KANA;
    case VK_RETURN: case VK_KP_ENTER:    return FK_RETURN;
    case VK_SPACE:                       return FK_SPACE;
    case VK_BACKSPACE: case VK_DELETE:   return FK_DEL;
    case VK_INSERT:                      return FK_INS;
    case VK_HOME:                        return FK_CLRHOME;
    case VK_UP:                          return FK_UP;
    case VK_DOWN:                        return FK_DOWN;
    case VK_LEFT:                        return FK_LEFT;
    case VK_RIGHT:                       return FK_RIGHT;
    case VK_MINUS:                       return FK_MINUS;
    case VK_EQUALS:                      return FK_CARET;
    case VK_GRAVEACCENT:                 return FK_YEN;
    case VK_LEFTBRACKET:                 return FK_AT;
    case VK_RIGHTBRACKET:                return FK_LBRACKET;
    case VK_BACKSLASH:                   return FK_RBRACKET;
    case VK_SEMICOLON:                   return FK_SEMICOLON;
    case VK_QUOTE:                       return FK_COLON;
    case VK_COMMA:                       return FK_COMMA;
    case VK_PERIOD:                      return FK_PERIOD;
    case VK_SLASH:                       return FK_SLASH;
    case VK_APPLICATION:                 return FK_UNDERSCORE;
    default:                             return FK_NONE;
  }
}


// the Famicom's Shift: what the newest held character wants, else the real Shift keys; true if
// it changed
static bool famiUpdateShift()
{
  int want = -1;
  bool left = false, right = false;
  for (int i = 0; i < s_famiHeldCount; ++i) {
    FamiHeld const & h = s_famiHeld[i];
    if (h.shift >= 0)
      want = h.shift;
    left |= h.key == FK_LSHIFT;
    right |= h.key == FK_RSHIFT;
  }
  if (want >= 0) {
    left = want == 1;
    right = false;
  }
  nes.setFamiKey(FK_LSHIFT, left);
  nes.setFamiKey(FK_RSHIFT, right);
  bool changed = (left || right) != s_famiShift;
  s_famiShift = left || right;
  return changed;
}


// a held key goes away: up on the Famicom, unless another held key is the same
static void famiRemove(int i)
{
  FamiKey key = s_famiHeld[i].key;
  s_famiHeld[i] = s_famiHeld[--s_famiHeldCount];
  bool still = false;
  for (int k = 0; k < s_famiHeldCount; ++k)
    still |= s_famiHeld[k].key == key && !s_famiHeld[k].wait;
  if (!still && key != FK_LSHIFT && key != FK_RSHIFT)
    nes.setFamiKey(key, false);
  famiUpdateShift();
}


static void famiKeyEvent(fabgl::VirtualKeyItem const & item)
{
  using namespace fabgl;
  VirtualKey position = positionKey(item);
  if (position == VK_PAUSE || position == VK_BREAK) {
    if (item.down)
      s_famiStopFrames = 6;
    return;
  }
  int i = 0;
  while (i < s_famiHeldCount && s_famiHeld[i].position != position)
    ++i;
  if (!item.down) {
    if (i == s_famiHeldCount)
      return;
    if (s_famiHeld[i].wait)
      s_famiHeld[i].released = true;  // typed when its wait is over
    else
      famiRemove(i);
    return;
  }
  if (i < s_famiHeldCount || s_famiHeldCount == (int)(sizeof(s_famiHeld) / sizeof(s_famiHeld[0])))
    return;                          // held already (the keyboard repeats), or too many keys

  // what the key types: a letter, a character of the layout, or else its place
  FamiKey key = FK_NONE;
  int8_t shift = -1;
  VirtualKey vk = item.vk;
  bool shifted;
  if ((vk >= VK_a && vk <= VK_z) || (vk >= VK_A && vk <= VK_Z)) {
    key = famiKeyOfChar(vk >= VK_a && vk <= VK_z ? 'a' + (vk - VK_a) : 'a' + (vk - VK_A), &shifted);
  } else if (item.ASCII > ' ' && item.ASCII < 0x7f && !item.CTRL && !item.LALT) {
    key = famiKeyOfChar(item.ASCII, &shifted);
    if (key != FK_NONE)
      shift = shifted;
  }
  if (key == FK_NONE)
    key = famiKeyAt(position);
  if (key == FK_NONE)
    return;
  FamiHeld & h = s_famiHeld[s_famiHeldCount++];
  h = { position, key, shift, 0, false, 0 };
  bool isShift = key == FK_LSHIFT || key == FK_RSHIFT;
  if (famiUpdateShift() && !isShift)
    h.wait = 2;
  else if (!isShift)
    nes.setFamiKey(key, true);
}


// every frame: keys waiting for Shift go down, typed ones that were let go meanwhile go up
static void famiHeldFrame()
{
  for (int i = 0; i < s_famiHeldCount; ++i) {
    FamiHeld & h = s_famiHeld[i];
    if (h.wait && !--h.wait) {
      nes.setFamiKey(h.key, true);
      if (h.released)
        h.hold = 3;
    } else if (h.hold && !--h.hold) {
      famiRemove(i--);
    }
  }
}


static void releaseFamiKeys()
{
  s_famiHeldCount = 0;
  s_famiShift = false;
  nes.releaseFamiKeys();
}


static void setFamiMode(bool on)
{
  s_famiMode = on;
  releaseFamiKeys();
  s_padState[0] = s_padState[1] = 0;
  showOverlay(on ? "FAMICOM KEYBOARD" : "KEYBOARD AS PADS");
  Serial.println(on ? "Keyboard: Famicom" : "Keyboard: pads");
}


// every frame: the STOP key's hold, and the next key of text typed over serial
static void famiKeysFrame()
{
  famiHeldFrame();
  if (s_famiStopFrames)
    nes.setFamiKey(FK_STOP, --s_famiStopFrames > 0);
  if (s_famiTyping.isEmpty())
    return;
  // Shift first, the key 2 frames later for 4 frames, then 4 frames up (36 after RETURN: BASIC is
  // busy with the line); Family BASIC loses a key that goes down as Shift changes
  int phase = s_famiTypingFrame++;
  bool shift;
  FamiKey key = famiKeyOfChar(s_famiTyping[0], &shift);
  if (phase == 0 && shift)
    nes.setFamiKey(FK_LSHIFT, true);
  else if (phase == 2)
    nes.setFamiKey(key, true);
  else if (phase == 6) {
    nes.setFamiKey(key, false);
    nes.setFamiKey(FK_LSHIFT, false);
  } else if (phase >= (key == FK_RETURN ? 42 : 10) - 1) {
    s_famiTyping = s_famiTyping.substring(1);
    s_famiTypingFrame = 0;
  }
}


// the keys the emulator keeps for itself
static bool reservedKey(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  return vk == VK_ESCAPE || vk == VK_F1 || vk == VK_F2 || vk == VK_F3 || vk == VK_F10 ||
         vk == VK_F5 || vk == VK_F6 || vk == VK_F7 || vk == VK_SCROLLLOCK || vk == VK_SYSREQ;
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


// a short name for a key (a position), at most 11 characters, as the layout labels it (arrows
// are the font's code page 437 arrows)
static String keyName(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  vk = layoutKey(vk);
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
  cv.drawText(&FONT_4x6, 104, AB_Y + 16, "SELECT");
  cv.drawText(&FONT_4x6, 134, AB_Y + 16, "START");
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
      // by place, so the keys stay when the layout changes
      fabgl::VirtualKeyItem item;
      vk = kb->getNextVirtualKey(&item, 0) && item.down ? positionKey(item) : fabgl::VK_NONE;
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
  if (path == LAYOUT_PATH)        return "Layout";
  if (path.endsWith(".sav"))      return "Save";
  if (path.endsWith(".cht"))      return "Codes";
  if (path.endsWith(".st1"))      return "State 1";
  if (path.endsWith(".st2"))      return "State 2";
  if (path.endsWith(".stn"))      return "State tmp";
  if (path.endsWith(".tp1"))      return "Tape data 1";
  if (path.endsWith(".tp2"))      return "Tape data 2";
  if (path.endsWith(".tp3"))      return "Tape data 3";
  if (path.endsWith(".tp4"))      return "Tape data 4";
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
      // by place, so the keys stay when the layout changes
      fabgl::VirtualKeyItem item;
      vk = kb->getNextVirtualKey(&item, 0) && item.down ? positionKey(item) : fabgl::VK_NONE;
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
      uint8_t bits = FONT_8x8.data[ch * 8 + row];
      if (bits & (0x80 >> ((x - 8) & 7)))
        px = fg;
    }
    line[x ^ 2] = px;                           // the DMA's byte order
  }
}


static void showOverlay(char const * text)
{
  snprintf(s_overlayText, sizeof(s_overlayText), "%s", text);
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
  int keyboard  = -1;       // the Family BASIC keyboard; -1 = by name ("BASIC")
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
    else if (key == "keyboard" && (yes || no))
      cfg.keyboard = yes;
    else if (key == "tape")
      ;                           // the tape trap, settled at build time (tools/tapetrap.py)
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


// SPIFFS names are at most 31 characters: a readable start of the ROM's name, then the CRC32 of
// its contents (worked out by tools/embed_roms.py), so a renamed ROM keeps its save
static String savePathFor(RomBlob const & rom, char const * ext)
{
  String stem;
  for (char const * p = rom.name; *p && stem.length() < 14; ++p) {
    char c = *p;
    if (c == '.' && (!strcasecmp(p, ".nes") || !strcasecmp(p, ".nsf")))
      break;
    stem += isalnum((unsigned char) c) ? c : '_';
  }
  char crc[10];
  snprintf(crc, sizeof(crc), "_%08x", (unsigned) rom.crc);
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


// Returns 1 when written, 0 when there was nothing to write, -1 when writing failed.
static int storeSave()
{
  if (s_savePath.isEmpty())
    return 0;
  uint32_t h = hashSaveRam();
  if (h == s_saveHash) {
    Serial.println("Save RAM unchanged, not written");
    return 0;
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
    return 1;
  }
  Serial.printf("Writing %s FAILED\n", s_savePath.c_str());
  return -1;
}


// the game's codes into the console, all on
static void applyCheats(RomBlob const & rom)
{
  std::vector<CheatEntry> list;
  loadCheats(rom, list);
  Cheat cheats[Nes::MAX_CHEATS];
  int cheatCount = 0;
  for (auto const & e : list)
    if (e.on && cheatCount < Nes::MAX_CHEATS && decodeCheat(e.code.c_str(), &cheats[cheatCount]))
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
// and only then replaces the slot, so a failed save leaves the old state. Returns nullptr or a message.
static char const * saveSlot(int idx, int slot)
{
  if (nes.isNsf())
    return "Music has no savestates";
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
  return ok ? nullptr : "Saving the state failed";
}


// Into the freshly loaded game. Returns nullptr or a message.
static char const * loadSlot(RomBlob const & rom, int slot)
{
  if (nes.isNsf())
    return "Music has no savestates";
  File f = SPIFFS.open(slotPath(rom, slot), "r");
  if (!f)
    return "The slot is empty";
  FileSource source(f);
  char const * err = nes.loadState(source);
  Serial.printf("State %s: %s\n", slotPath(rom, slot).c_str(), err ? err : "loaded");
  return err;
}


// the tape trap's state (see "Tape traps")
enum class TrapOp { NONE, SAVE, LOAD, WAIT_PC };
static TrapOp         s_trapOp = TrapOp::NONE;
static RomBlob const * s_trapRom;              // the game in the console (loadRom)
static std::vector<uint8_t> s_trapFile;        // Family BASIC: the file being saved or loaded
static bool           s_nsfFresh;              // music just started (loadRom): its time from 0


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
  nes.setFamiKeyboard(cfg.keyboard >= 0 ? cfg.keyboard : strstr(rom.name, "BASIC") != nullptr);
  applyCheats(rom);
  // the tape trap's stub (cart versions), and the call it makes
  s_trapRom = &rom;
  s_trapOp = TrapOp::NONE;
  Cheat patches[Nes::MAX_PATCHES];
  int patchCount = 0;
  if (TapeTrap const * t = rom.tapeTrap)
    for (int i = 0; i < t->patchCount && patchCount < Nes::MAX_PATCHES; ++i)
      patches[patchCount++] = { true, t->patches[i].addr, t->patches[i].value, t->patches[i].original };
  nes.setRomPatches(patches, patchCount);
  nes.enableVcCall(rom.tapeTrap && rom.tapeTrap->kind == TRAP_VC);
  nes.enablePortCall(rom.tapeTrap && rom.tapeTrap->kind == TRAP_FBASIC);
  s_trapFile.clear();
  if (nes.isNsf())
    nes.nsfStart(nes.nsf().firstSong);
  else
    nes.powerOn();
  s_nsfFresh = true;
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


// the buttons of both players, keyboard and real pads together; returns player 1's
static uint8_t updatePads(uint8_t serialButtons)
{
  uint8_t hw[2], first = 0;
  readHardwarePads(hw);
  for (int p = 0; p < 2; ++p) {
    uint8_t b = filterPad(s_padState[p] | hw[p] | (p ? 0 : serialButtons));
    nes.setButtons(p, b);
    if (!p)
      first = b;
  }
  nes.setMic(s_micState);
  return first;
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


///////////////////////////////////////////////////////////////////////////////
// Data Recorder (see nes/tape.h)
//
// One tape, in RAM; it stays in across games, as a cassette in the recorder would. The PC keeps
// the tapes (tools/tape.py): when a recording stops, the tape goes over serial as
//   Tape: data <hex, 64 bytes per line>   (as many lines as needed)
//   Tape: end <size>
// and 'U', the size, a newline and the image in hex put one in.

static Tape                   s_tape;
static constexpr size_t       TAPE_MAX = 32 * 1024;     // the longest recording, if memory allows

static void sendTape()
{
  uint8_t const * p = s_tape.data();
  size_t size = s_tape.size();
  for (size_t i = 0; i < size; i += 64) {
    char line[2 * 64 + 1];
    size_t n = std::min<size_t>(64, size - i);
    for (size_t k = 0; k < n; ++k)
      snprintf(line + 2 * k, 3, "%02x", p[i + k]);
    Serial.printf("Tape: data %s\n", line);
  }
  Serial.printf("Tape: end %u\n", (unsigned) size);
}


static void showTapeState(char const * what)
{
  char text[24];
  if (s_tape.mode() == Tape::STOPPED && !s_tape.empty())
    snprintf(text, sizeof(text), "TAPE %s %uB %us", what, (unsigned) s_tape.size(), (unsigned) s_tape.playedSeconds());
  else
    snprintf(text, sizeof(text), "TAPE %s", what);
  showOverlay(text);
  Serial.printf("Tape: %s\n", what);
}


static void tapeStop()
{
  bool recorded = s_tape.mode() == Tape::RECORDING;
  s_tape.stop();
  showTapeState("STOP");
  if (recorded)
    sendTape();
}


static void tapePlay()
{
  if (s_tape.mode() == Tape::RECORDING)
    tapeStop();
  if (s_tape.empty()) {
    showTapeState("EMPTY");
    return;
  }
  nes.tapePlay();
  showTapeState("PLAY");
}


static void tapeRecord()
{
  // leave room for the file system's buffers and the menu's pictures
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t capacity = std::min(TAPE_MAX, largest > 24 * 1024 ? largest - 24 * 1024 : 0);
  nes.tapeRecord(capacity);
  if (s_tape.mode() != Tape::RECORDING) {
    showTapeState("NO MEMORY");
    return;
  }
  showTapeState("REC");
  Serial.printf("Tape: up to %u bytes\n", (unsigned) capacity);
}


// Reads 'size' bytes in hex from serial into p (line ends and spaces between them are skipped);
// false if they stop coming for a second.
static bool readHex(uint8_t * p, long size)
{
  uint32_t last = millis();
  long n = 0;
  int high = -1;
  while (n < size && millis() - last < 1000) {
    int c = Serial.read();
    if (c < 0)
      continue;
    last = millis();
    int v = isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1;
    if (v < 0)
      continue;
    if (high < 0) {
      high = v;
    } else {
      p[n++] = high << 4 | v;
      high = -1;
    }
  }
  return n == size;
}


// 'U' came over serial: the size and the image in hex follow
static void receiveTape()
{
  long size = Serial.parseInt();
  if (size <= 0 || size > (long) TAPE_MAX) {
    Serial.println("Tape: bad size");
    return;
  }
  uint8_t * p = s_tape.loadBuffer(size);
  if (!p) {
    Serial.println("Tape: no memory");
    return;
  }
  if (char const * err = !readHex(p, size) ? "Tape: incomplete" : s_tape.load(nullptr, size)) {
    s_tape.loadBuffer(0);
    Serial.println(err);
    showTapeState("BAD");
    return;
  }
  Serial.printf("Tape: loaded %ld bytes\n", size);
  showTapeState("IN");
}


///////////////////////////////////////////////////////////////////////////////
// Tape traps (see TapeTrap in romindex.h, tools/tapetrap.py)
//
// A game with a tape trap (Mach Rider, Family BASIC) saves and loads its data in memory, without a
// tape. When it saves or loads, a line over the picture asks where: one of four slots in SPIFFS
// ("<rom>.tp1" to ".tp4") or the PC (tools/tape.py file, tools/fbasic.py); the game waits
// meanwhile. Mach Rider's file is its course; Family BASIC's is a tape file: the 128 byte header
// and the data. Over serial:
//   File: data <hex, 64 bytes per line>   (as many lines as needed)
//   File: end <size> <game>               a save, done
//   File: want <size> <game>              a load (size 0: any): 'F', the size, a newline and the
//                                         hex answer it

static int                  s_trapMessage;     // frames left for a message before the question
static constexpr size_t     FBASIC_MAX_FILE = 128 + 0x2000;

static size_t trapSize(TapeTrap const & t)
{
  size_t n = 0;
  for (int i = 0; i < t.blockCount; ++i)
    n += t.blocks[i].size;
  return n;
}


static uint8_t & trapByte(uint16_t addr)
{
  return addr < 0x2000 ? nes.ram[addr & 0x7ff] : nes.prgRam[addr & 0x1fff];
}


static String trapPath(int slot)
{
  char ext[5] = ".tp1";
  ext[3] = '0' + slot;
  return savePathFor(*s_trapRom, ext);
}


// a Family BASIC header's 16 bit field
static unsigned headerField(std::vector<uint8_t> const & file, int at)
{
  return file[at] | file[at + 1] << 8;
}


// BASIC's tape loop leaves its pointer ($AD/$AE) at the end of what it read, and LOAD takes that
// as the program's end; the end it reads to is $AF/$B0
static void fbasicPointers(uint16_t end)
{
  nes.ram[0xad] = nes.ram[0xaf] = end & 0xff;
  nes.ram[0xae] = nes.ram[0xb0] = end >> 8;
}


// what a save hands over
static std::vector<uint8_t> trapSaveData(TapeTrap const & t)
{
  if (t.kind == TRAP_FBASIC)
    return s_trapFile;
  std::vector<uint8_t> data;
  for (int i = 0; i < t.blockCount; ++i)
    for (int k = 0; k < t.blocks[i].size; ++k)
      data.push_back(trapByte(t.blocks[i].addr + k));
  return data;
}


// what a load takes: false if it does not fit the game
static bool trapLoadData(TapeTrap const & t, std::vector<uint8_t> const & data)
{
  if (t.kind == TRAP_FBASIC) {
    if (data.size() < 128 || data.size() > FBASIC_MAX_FILE || data.size() < 128 + headerField(data, 18))
      return false;
    s_trapFile = data;
    for (int k = 0; k < 128; ++k)              // the header now, the data when BASIC asks for it
      trapByte(t.blocks[0].addr + k) = data[k];
    fbasicPointers(t.blocks[0].addr + 128);
    return true;
  }
  if (data.size() != trapSize(t))
    return false;
  size_t n = 0;
  for (int i = 0; i < t.blockCount; ++i)
    for (int k = 0; k < t.blocks[i].size; ++k)
      trapByte(t.blocks[i].addr + k) = data[n++];
  return true;
}


static void trapMessage(char const * text)
{
  showOverlay(text);
  s_trapMessage = 90;
  Serial.printf("Trap: %s\n", text);
}


static void trapDone(char const * text, bool ok)
{
  trapMessage(text);
  s_trapOp = TrapOp::NONE;
  if (s_trapRom->tapeTrap->kind == TRAP_FBASIC)
    nes.finishPortCall(ok ? 0 : 2);            // BASIC's routines: carry set when it failed
  else
    nes.finishVcCall();
}


static void trapAsk(TrapOp op)
{
  s_trapOp = op;
  s_trapMessage = 0;
  Serial.printf("Trap: %s, 1-4 or p(c), ~ cancels\n", op == TrapOp::SAVE ? "save" : "load");
}


// Mach Rider (the VC's call): which call it is
static void trapStart(uint16_t call)
{
  TapeTrap const * t = s_trapRom ? s_trapRom->tapeTrap : nullptr;
  if (!t || (call != t->saveCall && call != t->loadCall)) {
    Serial.printf("Trap: unknown call from $%04X\n", call);
    nes.finishVcCall();
    return;
  }
  trapAsk(call == t->saveCall ? TrapOp::SAVE : TrapOp::LOAD);
}


// Family BASIC's tape routines: the header and the data, each on its own call
static void trapPortCall(uint8_t op)
{
  TapeTrap const & t = *s_trapRom->tapeTrap;
  uint16_t header = t.blocks[0].addr;
  switch (op) {
    case CALL_WRITE_HEADER:                    // kept until the data comes
      s_trapFile.assign(128, 0);
      for (int k = 0; k < 128; ++k)
        s_trapFile[k] = trapByte(header + k);
      nes.finishPortCall(0);
      return;
    case CALL_WRITE_DATA: {
      if (s_trapFile.size() < 128) {
        nes.finishPortCall(2);
        return;
      }
      unsigned size = headerField(s_trapFile, 18), at = headerField(s_trapFile, 20);
      s_trapFile.resize(128);
      for (unsigned k = 0; k < size && s_trapFile.size() < FBASIC_MAX_FILE; ++k)
        s_trapFile.push_back(trapByte(at + k));
      trapAsk(TrapOp::SAVE);
      return;
    }
    case CALL_READ_HEADER:
      trapAsk(TrapOp::LOAD);           // the header goes to $0500 when one is chosen (trapLoadData)
      return;
    case CALL_READ_DATA:
    case CALL_VERIFY_DATA: {
      bool ok = s_trapFile.size() >= 128 && s_trapFile.size() >= 128 + headerField(s_trapFile, 18);
      unsigned size = ok ? headerField(s_trapFile, 18) : 0, at = ok ? headerField(s_trapFile, 20) : 0;
      // into RAM or save RAM only
      ok &= (at + size <= 0x800) || (at >= 0x6000 && at + size <= 0x8000);
      for (unsigned k = 0; ok && k < size; ++k) {
        if (op == CALL_READ_DATA)
          trapByte(at + k) = s_trapFile[128 + k];
        else
          ok = trapByte(at + k) == s_trapFile[128 + k];
      }
      if (ok)
        fbasicPointers(at + size);
      nes.finishPortCall(ok ? 0 : 2);
      return;
    }
    default:
      Serial.printf("Trap: unknown call %d\n", op);
      nes.finishPortCall(2);
  }
}


// every frame while a trap waits: the question, unless a message shows
static void trapPrompt()
{
  if (s_trapMessage) {
    --s_trapMessage;
    return;
  }
  showOverlay(s_trapOp == TrapOp::SAVE ? "SAVE: 1-4, P=PC, ESC" :
              s_trapOp == TrapOp::LOAD ? "LOAD: 1-4, P=PC, ESC" : "WAITING FOR PC, ESC");
}


// a key while a trap waits: '1'-'4', 'p' or 27 (Esc)
static void trapKey(int c)
{
  TapeTrap const & t = *s_trapRom->tapeTrap;
  bool save = s_trapOp == TrapOp::SAVE;
  char text[24];
  if (c == 27) {
    trapDone(save ? "NOT SAVED" : "NOT LOADED", false);
    return;
  }
  if (s_trapOp == TrapOp::WAIT_PC)
    return;
  if (c >= '1' && c <= '4') {
    int slot = c - '0';
    File f = SPIFFS.open(trapPath(slot), save ? "w" : "r");
    if (!f) {
      snprintf(text, sizeof(text), save ? "CANNOT WRITE SLOT %d" : "SLOT %d IS EMPTY", slot);
      trapMessage(text);
      return;
    }
    if (save) {
      std::vector<uint8_t> data = trapSaveData(t);
      bool ok = f.write(data.data(), data.size()) == data.size();
      snprintf(text, sizeof(text), ok ? "SAVED TO SLOT %d" : "SLOT %d FAILED", slot);
      trapDone(text, ok);
      return;
    }
    std::vector<uint8_t> data(f.size());
    if (f.read(data.data(), data.size()) != data.size() || !trapLoadData(t, data)) {
      snprintf(text, sizeof(text), "SLOT %d IS BROKEN", slot);
      trapMessage(text);
      return;
    }
    snprintf(text, sizeof(text), "LOADED SLOT %d", slot);
    trapDone(text, true);
    return;
  }
  if (c == 'p' && save) {
    std::vector<uint8_t> data = trapSaveData(t);
    char line[2 * 64 + 1];
    for (size_t i = 0; i < data.size(); i += 64) {
      size_t n = std::min<size_t>(64, data.size() - i);
      for (size_t k = 0; k < n; ++k)
        snprintf(line + 2 * k, 3, "%02x", data[i + k]);
      Serial.printf("File: data %s\n", line);
    }
    Serial.printf("File: end %u %s\n", (unsigned) data.size(), t.game);
    trapDone("SENT TO PC", true);
    return;
  }
  if (c == 'p') {
    Serial.printf("File: want %u %s\n", t.kind == TRAP_FBASIC ? 0u : (unsigned) trapSize(t), t.game);
    s_trapOp = TrapOp::WAIT_PC;
    s_trapMessage = 0;
  }
}


// 'F' came over serial while waiting for the PC: the size and the data in hex follow
static void receiveTrapFile()
{
  TapeTrap const & t = *s_trapRom->tapeTrap;
  long size = Serial.parseInt();
  size_t most = t.kind == TRAP_FBASIC ? FBASIC_MAX_FILE : trapSize(t);
  std::vector<uint8_t> data(size > 0 && (size_t) size <= most ? size : 0);
  if (data.empty() || !readHex(data.data(), size) || !trapLoadData(t, data)) {
    Serial.printf("File: does not fit (%ld bytes)\n", size);
    trapMessage("BAD FILE FROM PC");
    return;
  }
  Serial.println("File: loaded");
  trapDone("LOADED FROM PC", true);
}


// a key from the keyboard as trapKey wants it, or 0
static int trapKeyOf(fabgl::VirtualKey vk)
{
  using namespace fabgl;
  if (vk >= VK_1 && vk <= VK_4)       return '1' + (vk - VK_1);
  if (vk >= VK_KP_1 && vk <= VK_KP_4) return '1' + (vk - VK_KP_1);
  if (vk == VK_p || vk == VK_P)       return 'p';
  if (vk == VK_ESCAPE)                return 27;
  return 0;
}


///////////////////////////////////////////////////////////////////////////////
// NSF music (see nes/nsf.h): instead of a picture, what plays. Left/Right on the pad change the
// song, Start pauses; SysRq as in games.

static bool     s_nsfPaused;
static uint32_t s_nsfFrames;        // played of the current song
static int      s_nsfShownSeconds;

// a level bar per channel, between the picture and the song line
static constexpr int  METER_Y = 118, METER_H = 36;
static char const *   METER_NAMES[Apu::METERS] = { "P1", "P2", "TR", "NO", "DM", "FD", "V1", "V2", "SA", "M1", "M2", "PC",
                                                   "S1", "S2", "S3", "N8", "N7", "N6", "N5", "N4", "N3", "N2", "N1",
                                                   "F1", "F2", "F3", "F4", "F5", "F6" };
static const uint8_t  METER_FULL[Apu::METERS] = { 15, 15, 15, 15, 64, 63, 15, 15, 31, 15, 15, 128, 255, 255, 255,
                                                  225, 225, 225, 225, 225, 225, 225, 225,
                                                  255, 255, 255, 255, 255, 255 };
static int            s_meterShown[Apu::METERS];   // pixels, falling back slowly
static int            s_meters[Apu::METERS];       // the channels the music has, as Apu::METER_
static int            s_meterCount;

// the channels to show, and where: spread over the width
static void nsfMeterLayout(int i, int * x, int * w)
{
  int step = s_meterCount ? 236 / s_meterCount : 36;
  if (step > 36)
    step = 36;
  *w = step * 2 / 3;
  *x = (256 - step * s_meterCount) / 2 + i * step + (step - *w) / 2;
}

static void nsfMeterChannels()
{
  s_meterCount = 0;
  for (int m = 0; m < Apu::METERS; ++m)
    if (m <= Apu::METER_DMC || (m == Apu::METER_FDS && (nes.nsf().chips & NSF_FDS)) ||
        (m >= Apu::METER_VRC6_1 && m <= Apu::METER_SAW && (nes.nsf().chips & NSF_VRC6)) ||
        (m >= Apu::METER_MMC5_1 && m <= Apu::METER_PCM && (nes.nsf().chips & NSF_MMC5)) ||
        (m >= Apu::METER_5B_A && m <= Apu::METER_5B_C && (nes.nsf().chips & NSF_5B)) ||
        (m >= Apu::METER_N163 && m < Apu::METER_VRC7 && (nes.nsf().chips & NSF_N163)) ||
        (m >= Apu::METER_VRC7 && (nes.nsf().chips & NSF_VRC7)))
      s_meters[s_meterCount++] = m;
}


// every frame: the bars, rising at once, falling 2 pixels a frame
static void drawNsfMeters()
{
  uint8_t level[Apu::METERS];
  nes.apu.takeMeters(level);
  fabgl::Canvas & cv = *s_canvas;
  static const Color COLOURS[Apu::METERS] = { Color::BrightGreen, Color::BrightGreen, Color::BrightCyan,
                                              Color::BrightYellow, Color::BrightMagenta, Color::BrightRed,
                                              Color::BrightWhite, Color::BrightWhite, Color::Yellow,
                                              Color::BrightBlue, Color::BrightBlue, Color::Cyan,
                                              Color::Magenta, Color::Magenta, Color::Magenta,
                                              Color::Green, Color::Green, Color::Green, Color::Green,
                                              Color::Green, Color::Green, Color::Green, Color::Green,
                                              Color::Red, Color::Red, Color::Red, Color::Red, Color::Red, Color::Red };
  for (int i = 0; i < s_meterCount; ++i) {
    int m = s_meters[i];
    int h = (s_nsfPaused ? 0 : level[m]) * METER_H / METER_FULL[m];
    if (h > METER_H)
      h = METER_H;
    if (h < s_meterShown[i] - 2)
      h = s_meterShown[i] - 2;
    if (h == s_meterShown[i])
      continue;
    int x, w;
    nsfMeterLayout(i, &x, &w);
    cv.setBrushColor(Color::Black);
    if (h < METER_H)
      cv.fillRectangle(x, METER_Y, x + w - 1, METER_Y + METER_H - 1 - h);
    cv.setBrushColor(COLOURS[m]);
    if (h > 0)
      cv.fillRectangle(x, METER_Y + METER_H - h, x + w - 1, METER_Y + METER_H - 1);
    s_meterShown[i] = h;
  }
}

// reset: a game powers on, music starts its song again
static void resetConsole()
{
  if (nes.isNsf()) {
    nes.nsfStart(nes.nsfSong());
    s_nsfFrames = 0;
  } else {
    nes.powerOn();
  }
}


static void drawNsfTime()
{
  char line[33];
  unsigned seconds = s_nsfFrames / nes.frameRate();
  snprintf(line, sizeof(line), "Song %3d of %-3d  %3u:%02u %s", nes.nsfSong(), nes.nsf().songs,
           seconds / 60, seconds % 60, s_nsfPaused ? "PAUSE" : "     ");
  fabgl::Canvas & cv = *s_canvas;
  cv.setBrushColor(Color::Black);
  cv.setPenColor(Color::BrightCyan);
  cv.drawText(0, 168, line);
  cv.waitCompletion();
  s_nsfShownSeconds = seconds;
}


static void drawNsfScreen()
{
  NsfInfo const & n = nes.nsf();
  fabgl::Canvas & cv = *s_canvas;
  cv.setBrushColor(Color::Black);
  cv.clear();
  auto centred = [&](int y, char const * text, Color color) {
    cv.setPenColor(color);
    cv.drawText((256 - (int) strlen(text) * 8) / 2, y, text);
  };
  centred(12, n.title, Color::BrightYellow);
  centred(24, n.artist, Color::White);
  centred(36, n.copyright, Color::BrightBlack);
  char chips[32];
  snprintf(chips, sizeof(chips), "2A03%s%s%s%s%s%s, %s", n.chips & NSF_FDS ? " + FDS" : "",
           n.chips & NSF_VRC6 ? " + VRC6" : "", n.chips & NSF_VRC7 ? " + VRC7" : "",
           n.chips & NSF_MMC5 ? " + MMC5" : "", n.chips & NSF_5B ? " + 5B" : "",
           n.chips & NSF_N163 ? " + N163" : "", nes.region() == Region::PAL ? "PAL" : "NTSC");
  centred(184, chips, Color::White);
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(0, 224, "\x1b\x1a:song  Start:pause  SysRq:menu");
  cv.waitCompletion();
  // the bars' names
  cv.setPenColor(Color::BrightBlack);
  nsfMeterChannels();
  for (int i = 0; i < s_meterCount; ++i) {
    int x, w;
    nsfMeterLayout(i, &x, &w);
    cv.drawText(x + (w - 16) / 2, METER_Y + METER_H + 2, METER_NAMES[s_meters[i]]);
  }
  cv.waitCompletion();
  memset(s_meterShown, 0, sizeof(s_meterShown));
  drawPicture(s_trapRom - g_roms, (256 - PICTURE_SIZE) / 2, 48);
  drawNsfTime();
}


// every frame: the pad's Left, Right and Start, as they go down
static void nsfFrame(uint8_t buttons)
{
  static uint8_t last;
  uint8_t pressed = buttons & ~last;
  last = buttons;
  int song = nes.nsfSong();
  if (pressed & (BTN_LEFT | BTN_RIGHT)) {
    int songs = nes.nsf().songs;
    song = pressed & BTN_RIGHT ? song % songs + 1 : (song + songs - 2) % songs + 1;
    nes.nsfStart(song);
    s_nsfFrames = 0;
    s_nsfPaused = false;
    Serial.printf("Song %d of %d\n", song, songs);
    drawNsfTime();
  }
  if (pressed & BTN_START) {
    s_nsfPaused = !s_nsfPaused;
    drawNsfTime();
  }
  if (!s_nsfPaused)
    ++s_nsfFrames;
  if ((int)(s_nsfFrames / nes.frameRate()) != s_nsfShownSeconds)
    drawNsfTime();
  drawNsfMeters();
}


static bool systemMenu();


// Runs the loaded game until it goes back to the ROM list (the system menu, or 'q' over serial).
static void runGame()
{
  auto kb = PS2.keyboard();
  kb->emptyVirtualKeyQueue();
  s_padState[0] = s_padState[1] = 0;
  s_micState = false;
  releaseFamiKeys();
  s_famiTyping = String();
  s_famiTypingFrame = 0;
  s_famiMode = false;
  if (nes.famiKeyboard())
    setFamiMode(true);
  bool music = nes.isNsf();
  nes.apu.metersOn = music;
  if (music) {
    if (s_nsfFresh)
      s_nsfFrames = 0;
    s_nsfFresh = false;
    s_nsfPaused = false;
    drawNsfScreen();
  }

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
  bool padMuted = false;

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
      fabgl::VirtualKeyItem item;
      if (!kb->getNextVirtualKey(&item, 0))
        break;
      auto vk = item.vk;
      bool down = item.down;
      if (s_trapOp != TrapOp::NONE) {           // the trap's question takes the keys
        if (int c = down ? trapKeyOf(vk) : 0)
          trapKey(c);
        continue;
      }
      if (down && vk == fabgl::VK_SYSREQ) {
        if (systemMenu())
          return;
        // the time the menu was open is not to be caught up
        next = esp_timer_get_time();
        kb->emptyVirtualKeyQueue();
        if (music)
          drawNsfScreen();
        continue;
      }
      if (down && vk == fabgl::VK_SCROLLLOCK && nes.famiKeyboard()) {
        setFamiMode(!s_famiMode);
        continue;
      }
      if (s_famiMode) {                         // every other key is the Famicom's
        famiKeyEvent(item);
        continue;
      }
      padKeyEvent(positionKey(item), down);
      if (!down)
        continue;
      if (vk == fabgl::VK_F1)
        resetConsole();
      if (vk == fabgl::VK_F10)
        toggleCheats();
      if (vk == fabgl::VK_F5)
        tapePlay();
      if (vk == fabgl::VK_F6)
        tapeRecord();
      if (vk == fabgl::VK_F7)
        tapeStop();
      if (vk == fabgl::VK_F2 || vk == fabgl::VK_F3) {
        s_volume = std::max(0, std::min(8, s_volume + (vk == fabgl::VK_F3 ? 1 : -1)));
        Serial.printf("Volume %d/8\n", s_volume);
      }
    }
    // serial console, for testing without a keyboard: 'q' leaves, a b e(select) s(tart) u d l r
    // tap a button for 10 frames
    if (Serial.available() && s_trapOp != TrapOp::NONE) {
      int c = Serial.read();
      if (c == 'F' && s_trapOp == TrapOp::WAIT_PC)
        receiveTrapFile();
      else
        trapKey(c == '~' ? 27 : c);
    } else if (Serial.available()) {
      int c = Serial.read();
      if (c == 'q')
        return;
      if (c == 'c')
        toggleCheats();
      if (c == 'Z' || c == 0x1a)
        sendScreenshot();
      if (c == 'P')
        tapePlay();
      if (c == 'R')
        tapeRecord();
      if (c == 'X')
        tapeStop();
      if (c == 'U')
        receiveTape();
      if (c == 'y') {                           // the system menu
        if (systemMenu())
          return;
        next = esp_timer_get_time();
        if (music)
          drawNsfScreen();
      }
      if (c == 'T' && nes.famiKeyboard()) {     // text for the Famicom keyboard, to the line end
        String line;                            // ('|' is RETURN)
        uint32_t start = millis();
        while (millis() - start < 1000) {
          int ch = Serial.read();
          if (ch < 0)
            continue;
          if (ch == '\n')
            break;
          line += ch == '|' ? '\n' : (char) ch;
        }
        s_famiTyping += line;
      }
      static char const keys[] = "abesudlr";
      if (char const * k = strchr(keys, c)) {
        // taps close together add up: "abes" holds all four
        serialButtons = (serialFrames ? serialButtons : 0) | 1 << (k - keys);
        serialFrames = 10;
      }
    }
    uint8_t pad = updatePads(serialFrames ? serialButtons : 0);
    // A+B+Select+Start on pad 1: the system menu; the buttons are then the game's again only once
    // all are let go
    if ((pad & 0x0f) == 0x0f && !padMuted) {
      padMuted = true;
      if (systemMenu())
        return;
      next = esp_timer_get_time();
      kb->emptyVirtualKeyQueue();
      if (music)
        drawNsfScreen();
    }
    if (padMuted) {
      if (pad)
        nes.setButtons(0, pad = 0);
      else
        padMuted = false;
    }
    famiKeysFrame();
    if (music)
      nsfFrame(pad);
    if (serialFrames)
      --serialFrames;

    int64_t t0 = esp_timer_get_time();
    if (s_overlayFrames)
      --s_overlayFrames;
    Tape::Mode tapeMode = s_tape.mode();
    if (!(music && s_nsfPaused))
      nes.runFrame(music ? false : draw);   // music: the screen is the player's
    int64_t t1 = esp_timer_get_time();
    if (s_trapOp == TrapOp::NONE) {
      if (uint16_t call = nes.vcCall())
        trapStart(call);
      else if (uint8_t op = nes.portCall())
        trapPortCall(op);
    }
    if (s_trapOp != TrapOp::NONE)
      trapPrompt();
    if (tapeMode != s_tape.mode()) {
      // the tape ran out, or the recording filled the memory
      if (tapeMode == Tape::RECORDING) {
        s_tape.stop();
        showTapeState("FULL");
        sendTape();
      } else {
        showTapeState("END");
      }
    }
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
      // (the IDF's printf, newlib nano, has no long long)
      Serial.printf("%.1f fps (%.1f drawn), emulation %d us/frame avg, %d worst (budget %d)\n",
                    frames * 1e6 / (now - statStart), drawn * 1e6 / (now - statStart),
                    (int)(busy / frames), (int) worst, (int) period);
      Serial.printf("  sound buffer %u samples, lowest %u (target %u)\n", (unsigned) ringFill(), (unsigned) minFill, (unsigned) audioTarget);
      minFill = RING_SIZE;
      if (drawn)
        Serial.printf("  core 0 drawing %u us/frame, core 1 waiting for it %u us/frame\n",
                      (unsigned)(drawCycles / drawn / 240), (unsigned)(waitCycles / drawn / 240));
      waitCycles = drawCycles = 0;
      statStart = now;
      busy = worst = 0;
      frames = drawn = 0;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// the main menu (see MenuMockup.txt): the games with the selected one's picture and facts, Start
// opens the options over it. Drawn through the text grid (ui.h), which redraws only what changed.

// The game in the console while the menu shows: leaving it through the system menu pauses it
// (nothing is copied, the console just waits), A on it carries on. Starting or loading another
// game ends it. -1: none.
static int s_paused = -1;

// what a menu reads: the pad's buttons, from the keys mapped to them, real pads or the serial console
enum MenuKey { MK_NONE, MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_A, MK_B, MK_START, MK_SELECT,
               MK_PAGEUP, MK_PAGEDOWN, MK_ESC, MK_TAB, MK_DEL, MK_F1, MK_ROM, MK_1, MK_2, MK_3 };
static int s_serialRom;              // MK_ROM: the game's number sent over serial
static uint8_t s_menuPads;           // the real pads' buttons as last seen, for new presses

static fabgl::VirtualKey s_menuHeld[2 * PAD_KEYS];   // keys held when a menu opened
static int s_menuHeldCount;

// A menu opens: buttons held now (the pad's menu combination) are not presses for it, also not
// the keyboard's repeats of their keys, until they are let go.
static void menuKeyStart()
{
  uint8_t hw[2];
  readHardwarePads(hw);
  s_menuPads = filterPad(hw[0] | hw[1]);
  auto kb = PS2.keyboard();
  s_menuHeldCount = 0;
  for (int p = 0; p < 2; ++p)
    for (int b = 0; b < PAD_KEYS; ++b)
      if (s_padKeys[p][b] != fabgl::VK_NONE && kb->isVKDown(s_padKeys[p][b]))
        s_menuHeld[s_menuHeldCount++] = s_padKeys[p][b];
}

// The framebuffer to the serial console, a line of 256 characters '0' + raw pixel & 63 each
// (tools/screenshot.py makes a PNG of it).
static void sendScreenshot()
{
  char line[257];
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 256; ++x)
      line[x] = '0' + (s_lines[y][x ^ 2] & 63);
    line[256] = 0;
    Serial.printf("Screen: %s\n", line);
  }
}


// Waits up to 100 ms for a button. Keys: arrows, Enter = A, Esc or SysRq = MK_ESC (B in most
// menus, but where B has a job of its own it still goes back) besides the pad's keys.
// Serial: u d l r a b e(select) s(tart) as in a game, a number picks that game, Z a screenshot.
static MenuKey menuKey()
{
  static const MenuKey buttons[8] = { MK_A, MK_B, MK_SELECT, MK_START, MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT };
  static uint32_t repeatAt;
  auto kb = PS2.keyboard();
  for (uint32_t start = millis(); millis() - start < 100; ) {
    if (kb->virtualKeyAvailable()) {
      fabgl::VirtualKeyItem item;
      if (!kb->getNextVirtualKey(&item, 0))
        continue;
      // a key held since the menu opened: its repeats are no presses, its release ends that
      bool held = false;
      for (int i = 0; i < s_menuHeldCount; ++i)
        if (s_menuHeld[i] == canonKey(positionKey(item))) {
          held = true;
          if (!item.down)
            s_menuHeld[i] = s_menuHeld[--s_menuHeldCount];
          break;
        }
      if (held || !item.down)
        continue;
      using namespace fabgl;
      switch (item.vk) {
        case VK_UP:       case VK_KP_UP:       return MK_UP;
        case VK_DOWN:     case VK_KP_DOWN:     return MK_DOWN;
        case VK_LEFT:     case VK_KP_LEFT:     return MK_LEFT;
        case VK_RIGHT:    case VK_KP_RIGHT:    return MK_RIGHT;
        case VK_PAGEUP:   case VK_KP_PAGEUP:   return MK_PAGEUP;
        case VK_PAGEDOWN: case VK_KP_PAGEDOWN: return MK_PAGEDOWN;
        case VK_RETURN:   case VK_KP_ENTER:    return MK_A;
        case VK_ESCAPE:   case VK_SYSREQ:      return MK_ESC;
        case VK_TAB:                           return MK_TAB;
        case VK_DELETE:   case VK_KP_DELETE:   return MK_DEL;
        case VK_F1:                            return MK_F1;
        default: break;
      }
      VirtualKey vk = canonKey(positionKey(item));
      for (int p = 0; p < 2; ++p)
        for (int b = 0; b < 8; ++b)
          if (s_padKeys[p][b] == vk)
            return buttons[b];
      if (vk >= VK_1 && vk <= VK_3)               // the digit keys not mapped to the pad
        return (MenuKey)(MK_1 + (vk - VK_1));
      continue;
    }
    if (Serial.available()) {
      int c = Serial.peek();
      if (isdigit(c)) {
        s_serialRom = Serial.parseInt();
        return MK_ROM;
      }
      Serial.read();
      if (c == 'Z' || c == 0x1a)
        sendScreenshot();
      if (c == '!')
        return MK_F1;
      static char const keys[] = "abesudlr";
      if (char const * k = strchr(keys, c))
        return buttons[k - keys];
      continue;
    }
    // real pads: a new press, or a held direction repeating
    uint8_t hw[2];
    readHardwarePads(hw);
    uint8_t pads = filterPad(hw[0] | hw[1]), pressed = pads & ~s_menuPads;
    s_menuPads = pads;
    if (pressed)
      repeatAt = millis() + 400;
    else if ((pads & 0xf0) && (int32_t)(millis() - repeatAt) >= 0) {
      pressed = pads & 0xf0;
      repeatAt = millis() + 120;
    }
    for (int b = 0; b < 8; ++b)
      if (pressed & (1 << b))
        return buttons[b];
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return MK_NONE;
}


// "Are you sure?": a box over whatever is on the screen; lines split at '\n'. A is yes, B no.
static bool confirm(char const * text)
{
  std::vector<String> lines;
  for (char const * p = text; ; ) {
    char const * end = strchr(p, '\n');
    lines.push_back(end ? String(p).substring(0, end - p) : String(p));
    if (!end)
      break;
    p = end + 1;
  }
  int w = 18;
  for (auto const & l : lines)
    w = std::max(w, std::min(ui::COLS, l.length() + 4));
  int h = lines.size() + 4, col = (ui::COLS - w) / 2, row = (ui::ROWS - h) / 2;
  ui::box(col, row, w, h);
  for (int i = 0; i < (int) lines.size(); ++i)
    ui::put(col + 2, row + 1 + i, lines[i].c_str());
  ui::divider(col, row + h - 3, w);
  ui::hint(col + 2, row + h - 2, "A: Yes   B: No");
  for (;;) {
    ui::flush();
    switch (menuKey()) {
      case MK_A:                return true;
      case MK_B: case MK_START: case MK_ESC: return false;
      default:                  break;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// cheat codes (see MenuMockup.txt): a list per game, each code on or off with a comment, kept in a
// text file next to the saves, a line per code: "1<tab>8F56 2A 3B<tab>Invincible" (on, code, comment)

static constexpr int CHEAT_ROWS = 24, CODE_LEN = 10, COMMENT_LEN = 16;

static void loadCheats(RomBlob const & rom, std::vector<CheatEntry> & list)
{
  list.clear();
  String path = savePathFor(rom, ".cht");
  if (!SPIFFS.exists(path))
    return;
  File f = SPIFFS.open(path, "r");
  while (f && f.available() && (int) list.size() < Nes::MAX_CHEATS) {
    String line = f.readStringUntil('\n');
    int tab1 = line.indexOf('\t'), tab2 = tab1 < 0 ? -1 : line.indexOf('\t', tab1 + 1);
    if (tab2 < 0)
      continue;                   // not a line of this format
    CheatEntry e { line[0] == '1', line.substring(tab1 + 1, tab2), line.substring(tab2 + 1) };
    e.comment.trim();
    Cheat c;
    if (decodeCheat(e.code.c_str(), &c))
      list.push_back(e);
  }
}


static void storeCheats(RomBlob const & rom, std::vector<CheatEntry> const & list)
{
  String path = savePathFor(rom, ".cht");
  s_soundGen->play(false);          // flash writes stop the cache, and the sound interrupt with it
  if (list.empty()) {
    if (SPIFFS.exists(path))
      SPIFFS.remove(path);
  } else {
    File f = SPIFFS.open(path, "w");
    for (auto const & e : list)
      f.print(String(e.on ? "1\t" : "0\t") + e.code + "\t" + e.comment + "\n");
    f.close();
  }
  s_soundGen->play(true);
}


// whether a pad button is held now (a key mapped to it, or a real pad), for B+Right
static bool buttonHeld(int bit)
{
  auto kb = PS2.keyboard();
  int b = __builtin_ctz(bit);
  return (s_menuPads & bit) || kb->isVKDown(s_padKeys[0][b]) || kb->isVKDown(s_padKeys[1][b]);
}


// The cheat entry box over the list: a code and a comment, typed on the keyboard or picked from
// the drawn one with the pad (F1 switches, remembered until the next power on). Returns false when
// aborted; else *e has the code in its canonical form.
static bool cheatEntry(CheatEntry * e)
{
  using namespace ui;
  static int keyboardMode = -1;       // -1: by whether a keyboard is there
  if (keyboardMode < 0)
    keyboardMode = PS2.keyboard()->isKeyboardAvailable();
  // the drawn keyboard: per row the first column and the keys; '\x01' is space, '\x02' abort,
  // '\x03' OK
  struct Key { int col; char ch; };
  static const struct { int col; char const * keys; } layout[6] = {
    { 5, "1234567890" }, { 5, "QWERTYUIOP" }, { 7, "ASDFGHJKL" }, { 9, "ZXCVBNM\x01" },
    { 4, "!\"$%&+-*/=<>" }, { 4, "()[]_.,:;\x02\x03" },
  };
  auto keyCol = [&](int r, int i) {
    int col = layout[r].col + 2 * i;
    if (r == 5 && i >= 9)              // [X] [/] after a gap
      col = i == 9 ? 23 : 27;
    if (r == 3 && i == 7)              // [Spc]
      col = 23;
    return col;
  };
  auto keyCount = [&](int r) { return (int) strlen(layout[r].keys); };
  String fields[2] = { e->code, e->comment };
  int lens[2] = { CODE_LEN, COMMENT_LEN };
  int field = 0, kr = 0, ki = 0;
  static constexpr int ROW = 7, H = 16;
  auto kb = PS2.keyboard();
  kb->emptyVirtualKeyQueue();

  auto type = [&](char ch) {
    if ((int) fields[field].length() >= lens[field])
      return;
    if (field == 0 && !isalnum((unsigned char) ch) && ch != ' ')
      return;                          // codes: hex digits and Game Genie letters
    fields[field] += field == 0 ? (char) toupper(ch) : ch;
  };
  auto backspace = [&]() {
    if (fields[field].length())
      fields[field].remove(fields[field].length() - 1);
  };
  // OK: true when the code is valid (then it is in *e)
  auto accept = [&]() {
    Cheat c;
    if (!decodeCheat(fields[0].c_str(), &c)) {
      message(BAD, "Not a valid code");
      return false;
    }
    char canon[16];
    formatCheat(c, canon, sizeof(canon));
    e->code = canon;
    e->comment = fields[1];
    e->comment.trim();
    return true;
  };

  for (;;) {
    box(0, ROW, COLS, H);
    for (int f = 0; f < 2; ++f) {
      int r = ROW + 1 + f;
      put(1, r, f ? "Comment:" : "Code:");
      bool on = f == field;
      putc(10, r, '[', on ? YELLOW : GREY);
      for (int i = 0; i < lens[f]; ++i) {
        bool filled = i < fields[f].length();
        putc(11 + i, r, filled ? fields[f][i] : on && i == fields[f].length() ? '_' : '.', filled ? TEXT : on && i == fields[f].length() ? YELLOW : GREY);
      }
      putc(11 + lens[f], r, ']', on ? YELLOW : GREY);
    }
    divider(0, ROW + 3, COLS);
    for (int r = 0; r < 6; ++r)
      for (int i = 0; i < keyCount(r); ++i) {
        char ch = layout[r].keys[i];
        bool sel = !keyboardMode && r == kr && i == ki;
        uint8_t bg = sel ? SEL_BG : BLACK, fg = sel ? SEL_TEXT : TEXT;
        int col = keyCol(r, i);
        if (ch == '\x01')
          put(col, ROW + 4 + r, "[Spc]", fg, bg);
        else if (ch == '\x02' || ch == '\x03') {
          put(col, ROW + 4 + r, "[ ]", fg, bg);
          putc(col + 1, ROW + 4 + r, ch == '\x02' ? G_CROSS : G_CHECK, sel ? SEL_TEXT : ch == '\x02' ? RED : GREEN, bg);
        } else
          putc(col, ROW + 4 + r, ch, fg, bg);
      }
    divider(0, ROW + 10, COLS);
    hint(1, ROW + 11, "+: Move Crsr    A: Key    B: <");
    hint(4, ROW + 12, "Select|Tab: Change Field");
    hint(3, ROW + 13, "F1: Toggle Keyboard Joypad");
    put(14, ROW + 13, "Keyboard", keyboardMode ? GREEN : GREY);
    put(23, ROW + 13, "Joypad", keyboardMode ? GREY : GREEN);
    hint(1, ROW + 14, "[X]|ESC: Abort   [V]|Enter: OK");
    putc(2, ROW + 14, G_CROSS, RED);
    putc(19, ROW + 14, G_CHECK, GREEN);
    flush();

    if (keyboardMode) {
      // typing: the layout's characters; Enter, Esc, Tab, Backspace, F1. Over serial: characters
      // type, Enter is OK, ~ aborts, | changes the field, ^ deletes, ! switches to the pad
      int ch = 0;
      fabgl::VirtualKey vk = fabgl::VK_NONE;
      if (Serial.available()) {
        int c = Serial.read();
        if (c == 0x1a)              sendScreenshot();     // Ctrl+Z: Z is a letter to type here
        else if (c == '\r' || c == '\n') vk = fabgl::VK_RETURN;
        else if (c == '~')          vk = fabgl::VK_ESCAPE;
        else if (c == '|')          vk = fabgl::VK_TAB;
        else if (c == '^')          vk = fabgl::VK_BACKSPACE;
        else if (c == '!')          vk = fabgl::VK_F1;
        else if (c >= 32 && c < 127) ch = c;
      } else if (kb->virtualKeyAvailable()) {
        fabgl::VirtualKeyItem item;
        if (kb->getNextVirtualKey(&item, 0) && item.down) {
          vk = item.vk;
          if (item.ASCII >= 32 && item.ASCII < 127)
            ch = item.ASCII;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      if (ch) {
        type(ch);
        continue;
      }
      switch (vk) {
        case fabgl::VK_RETURN: case fabgl::VK_KP_ENTER:
          if (accept())
            return true;
          break;
        case fabgl::VK_ESCAPE:    return false;
        case fabgl::VK_TAB:       field ^= 1; break;
        case fabgl::VK_BACKSPACE: backspace(); break;
        case fabgl::VK_F1:        keyboardMode = 0; menuKeyStart(); break;
        default: break;
      }
      continue;
    }

    // the pad on the drawn keyboard
    auto nearest = [&](int r) {
      int col = keyCol(kr, ki), best = 0;
      for (int i = 1; i < keyCount(r); ++i)
        if (abs(keyCol(r, i) - col) < abs(keyCol(r, best) - col))
          best = i;
      return best;
    };
    switch (menuKey()) {
      case MK_LEFT:  ki = (ki + keyCount(kr) - 1) % keyCount(kr); break;
      case MK_RIGHT: ki = (ki + 1) % keyCount(kr); break;
      case MK_UP:    { int r = (kr + 5) % 6; ki = nearest(r); kr = r; break; }
      case MK_DOWN:  { int r = (kr + 1) % 6; ki = nearest(r); kr = r; break; }
      case MK_B:     backspace(); break;
      case MK_SELECT: case MK_TAB: field ^= 1; break;
      case MK_ESC:   return false;
      case MK_F1:    keyboardMode = 1; kb->emptyVirtualKeyQueue(); break;
      case MK_A: {
        char ch = layout[kr].keys[ki];
        if (ch == '\x02')
          return false;
        if (ch == '\x03') {
          if (accept())
            return true;
        } else
          type(ch == '\x01' ? ' ' : ch);
        break;
      }
      default:
        break;
    }
  }
}


// The cheat list: Up/Down select, A edits (on NEW: a new code), B switches one on and off, B+Right
// (or Delete) deletes it, Start (Esc) goes back. Changes are written when leaving.
static void editCheats(RomBlob const & rom)
{
  using namespace ui;
  std::vector<CheatEntry> list, before;
  loadCheats(rom, list);
  before = list;
  int sel = 0, toggled = -1;          // toggled: the row B just switched (B+Right takes that back)
  invalidate();
  menuKeyStart();
  messageArea(0, COLS);

  auto changed = [&]() {
    if (list.size() != before.size())
      return true;
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].on != before[i].on || list[i].code != before[i].code || list[i].comment != before[i].comment)
        return true;
    return false;
  };

  for (;;) {
    int rows = std::min((int) list.size() + 1, CHEAT_ROWS);      // with NEW while there is room
    sel = std::max(0, std::min(sel, rows - 1));
    clear();
    putc(0, 0, G_V, LINE);
    putc(COLS - 1, 0, G_V, LINE);
    char text[33];
    snprintf(text, COLS - 1, "%s", rom.title);
    put(1, 0, text, GREEN);
    divider(0, 1, COLS);
    putc(0, 1, G_BL, LINE);
    putc(COLS - 1, 1, G_BR, LINE);
    for (int i = 0; i < rows; ++i) {
      bool on = i == sel;
      uint8_t bg = on ? SEL_BG : BLACK, fg = on ? SEL_TEXT : TEXT;
      fill(0, 2 + i, COLS, 1, ' ', fg, bg);
      if (i == (int) list.size()) {
        put(1, 2 + i, "NEW", on ? SEL_TEXT : GREY, bg);
        continue;
      }
      CheatEntry const & e = list[i];
      putc(0, 2 + i, e.on ? G_CHECK : G_CROSS, e.on ? GREEN : GREY, bg);
      put(1, 2 + i, e.code.c_str(), fg, bg);
      put(12, 2 + i, e.comment.c_str(), fg, bg);
    }
    box(0, ROWS - 4, COLS, 5);        // its bottom line is off the screen
    hint(1, ROWS - 3, "^v: Move Crsr          A: Edit");
    hint(1, ROWS - 2, "B: Toggle          B+>: Delete");
    hint(1, ROWS - 1, "Select: Search     Start: Back");
    flush();

    MenuKey key = menuKey();
    bool onCode = sel < (int) list.size();
    if (key != MK_NONE && key != MK_B && key != MK_RIGHT)
      toggled = -1;
    switch (key) {
      case MK_UP:   sel = (sel + rows - 1) % rows; break;
      case MK_DOWN: sel = (sel + 1) % rows; break;
      case MK_B:
        if (onCode) {
          list[sel].on = !list[sel].on;
          toggled = sel;
        }
        break;
      case MK_RIGHT: case MK_DEL:
        if (!onCode || (key == MK_RIGHT && !buttonHeld(BTN_B)))
          break;
        if (toggled == sel)           // the B of B+Right switched it: back as it was
          list[sel].on = !list[sel].on;
        toggled = -1;
        if (confirm("Delete this code?")) {
          list.erase(list.begin() + sel);
          message(NEUTRAL, "Code deleted");
        }
        break;
      case MK_A: {
        CheatEntry e = onCode ? list[sel] : CheatEntry { true, "", "" };
        if (cheatEntry(&e)) {
          if (onCode)
            list[sel] = e;
          else
            list.push_back(e);
        }
        break;
      }
      case MK_SELECT:
        message(NEUTRAL, "The search comes next");
        break;
      case MK_START: case MK_ESC:
        if (changed()) {
          storeCheats(rom, list);
          message(GOOD, "Cheats saved");
        }
        return;
      default:
        break;
    }
  }
}


// the region menu choice's name
static char const * regionChoiceName()
{
  static char const * const names[] = { "Auto", "NTSC", "PAL" };
  return names[(int) s_regionChoice];
}


static bool wantsKeyboard(RomBlob const & rom, RomConfig const & cfg)
{
  return cfg.keyboard >= 0 ? cfg.keyboard : strstr(rom.name, "BASIC") != nullptr;
}


// what the menu tells about a game, gathered when it is selected
struct GameFacts {
  int          idx = -1;
  bool         music;
  char         size[12];
  char         mapper[12];
  bool         supported;
  char const * features[6];
  int          featureCount;
  int          cheats;
  bool         slots[2];
};

static void gatherFacts(int idx, GameFacts & f)
{
  RomBlob const & rom = g_roms[idx];
  f.idx = idx;
  f.featureCount = 0;
  f.cheats = 0;
  f.slots[0] = f.slots[1] = false;
  f.supported = true;
  f.music = isNsf(rom.data, rom.size);
  if (f.music) {
    NsfInfo info;
    if (parseNsf(rom.data, rom.size, &info))
      info.chips = 0;
    snprintf(f.size, sizeof(f.size), "%uk", (unsigned)((rom.size - 128 + 1023) / 1024));
    snprintf(f.mapper, sizeof(f.mapper), "NSF");
    static char const * const chips[] = { "VRC6", "VRC7", "FDS", "MMC5", "Namco163", "Sunsoft5B" };
    for (int i = 0; i < 6; ++i)
      if (info.chips & (1 << i))
        f.features[f.featureCount++] = chips[i];
    return;
  }
  Cart cart;
  if (parseRom(rom.data, rom.size, &cart)) {
    snprintf(f.size, sizeof(f.size), "?");
    snprintf(f.mapper, sizeof(f.mapper), "?");
    f.supported = false;
    return;
  }
  RomInfo const & info = cart.info;
  snprintf(f.size, sizeof(f.size), "%dk", (info.prgSize + info.chrSize) / 1024);
  static const struct { int number; char const * name; } names[] = {
    { 0, "NROM" }, { 1, "MMC1" }, { 2, "UxROM" }, { 3, "CNROM" }, { 4, "MMC3" }, { 5, "MMC5" },
    { 7, "AxROM" }, { 9, "MMC2" }, { 10, "MMC4" }, { 69, "FME-7" }, { 71, "Camerica" }, { 85, "VRC7" },
  };
  snprintf(f.mapper, sizeof(f.mapper), "#%d", info.mapper);
  for (auto const & n : names)
    if (n.number == info.mapper)
      snprintf(f.mapper, sizeof(f.mapper), "%s", n.name);
  f.supported = mapperSupported(info.mapper);

  RomConfig cfg = parseConfig(rom.config);
  bool battery = cfg.save >= 0 ? cfg.save : info.battery;
  if (battery)
    f.features[f.featureCount++] = "SRAM+Bat";
  else if (info.nes20 && (rom.data[10] & 0x0f))
    f.features[f.featureCount++] = "SRAM";
  if (!info.chrSize)
    f.features[f.featureCount++] = "CHRRAM";
  if (rom.tapeTrap)
    f.features[f.featureCount++] = "Tape";
  if (wantsKeyboard(rom, cfg))
    f.features[f.featureCount++] = "Keyboard";

  std::vector<CheatEntry> list;
  loadCheats(rom, list);
  f.cheats = std::count_if(list.begin(), list.end(), [](CheatEntry const & e) { return e.on; });
  f.slots[0] = slotUsed(rom, 1);
  f.slots[1] = slotUsed(rom, 2);
}


// main menu layout: the list's rows, the picture's cells
static constexpr int LIST_ROW = 12, LIST_ROWS = 15;
static constexpr int PIC_COL = 4, PIC_ROW = 3;

static void drawLogo(int col, int row)
{
  // 4 x 2 glyphs of the font (0xEC-0xEF over 0xFC-0xFF): a cartridge in light grey, the name in red
  ui::put(col, row, "\xec\xed", ui::BRIGHT);
  ui::put(col, row + 1, "\xfc\xfd", ui::BRIGHT);
  ui::put(col + 2, row, "\xee\xef", ui::RED);
  ui::put(col + 2, row + 1, "\xfe\xff", ui::RED);
}


static void drawMainMenu(int selected, int top, GameFacts const & f)
{
  using namespace ui;
  clear();
  messageArea(6, COLS - 6);           // next to the logo
  drawLogo(1, 0);
  char text[33];
  snprintf(text, sizeof(text), "TV:%s", regionChoiceName());
  put(COLS - strlen(text), 0, text);
  put(COLS - 5, 1, "ESP32");

  // the list, joined to the picture's frame
  box(3, 2, 10, 10);
  box(0, 11, COLS, ROWS - 11);
  putc(3, 11, G_UT, LINE);
  putc(12, 11, G_UT, LINE);
  divider(0, ROWS - 3, COLS);
  hint(1, ROWS - 2, "+: Cursor  Start: Menu  A: Run");

  if (!g_romCount) {
    put(2, LIST_ROW + 1, "No ROMs built in.", RED);
    put(2, LIST_ROW + 3, "Put .nes files in roms/ and");
    put(2, LIST_ROW + 4, "run pio run -t upload");
    return;
  }
  picture(PIC_COL, PIC_ROW, PICTURE_SIZE / 8, PICTURE_SIZE / 8);

  // the selected game's facts
  put(18, 2, "Size:");
  put(24, 2, f.size);
  put(16, 3, "Mapper:");
  put(24, 3, f.mapper, f.supported ? TEXT : RED);
  put(14, 4, f.music ? "   Chips:" : "Features:");
  if (!f.featureCount)
    put(24, 4, "-", GREY);
  for (int i = 0; i < f.featureCount && i < 4; ++i)
    put(24, 4 + i, f.features[i]);
  if (!f.music) {
    put(14, 9, "Cheats   :");
    if (f.cheats) {
      snprintf(text, sizeof(text), "%d", f.cheats);
      put(25, 9, text);
    } else
      putc(25, 9, G_CROSS, GREY);
    put(14, 10, "Savestate:");
    for (int s = 0; s < 2; ++s)
      putc(25 + 2 * s, 10, f.slots[s] ? G_CHECK : G_CROSS, f.slots[s] ? GREEN : GREY);
  }

  // the games: an icon, the name; the paused one in green with a play arrow
  for (int r = 0; r < LIST_ROWS && top + r < g_romCount; ++r) {
    int idx = top + r;
    RomBlob const & rom = g_roms[idx];
    bool sel = idx == selected, paused = idx == s_paused;
    uint8_t bg = sel ? SEL_BG : BLACK;
    fill(1, LIST_ROW + r, COLS - 2, 1, ' ', TEXT, bg);
    putc(1, LIST_ROW + r, paused ? G_PLAY : isNsf(rom.data, rom.size) ? G_NOTE : G_MODULE,
         paused ? GREEN : sel ? SEL_TEXT : TEXT, bg);
    snprintf(text, COLS - 2, "%s", rom.title);
    put(2, LIST_ROW + r, text, sel ? SEL_TEXT : paused ? GREEN : TEXT, bg);
  }
  if (top > 0)
    putc(COLS - 1, LIST_ROW, G_UP, TEXT);
  if (top + LIST_ROWS < g_romCount)
    putc(COLS - 1, LIST_ROW + LIST_ROWS - 1, G_DOWN, TEXT);
}


// the About screen, until a button; 1, 2 and 3 play the message sounds (a hidden sound test,
// also over serial)
static void aboutScreen()
{
  using namespace ui;
  static char flashFree[16];
  if (!flashFree[0]) {
    // what the firmware leaves of its partition (the image's size takes a read of it all)
    esp_partition_t const * app = esp_ota_get_running_partition();
    esp_image_metadata_t meta = { };
    esp_partition_pos_t pos = { app->address, app->size };
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK)
      snprintf(flashFree, sizeof(flashFree), "%u KB", (unsigned)((app->size - meta.image_len) / 1024));
    else
      snprintf(flashFree, sizeof(flashFree), "?");
  }
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);
  esp_partition_t const * spiffs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);

  clear();
  drawLogo(1, 0);
  put(8, 0, "ESPNES (c) 2026", YELLOW);
  put(7, 1, "by Bastet and Claude", YELLOW);
  char text[33];
  snprintf(text, sizeof(text), "MCU Version  : ESP32 rev %d.%d", chip.revision / 100, chip.revision % 100);
  put(0, 3, text);
  snprintf(text, sizeof(text), "MCU Core     : Xtensa LX6 x%d", chip.cores);
  put(0, 4, text);
  put(0, 5, "Video Core   : FabGL");
  snprintf(text, sizeof(text), "Flash Size   : %u KB", (unsigned)(flashSize / 1024));
  put(0, 6, text);
  snprintf(text, sizeof(text), "Flash Free   : %s", flashFree);
  put(0, 7, text);
  snprintf(text, sizeof(text), "SPIFFS Size  : %u KB", spiffs ? (unsigned)(spiffs->size / 1024) : 0);
  put(0, 8, text);
  put(0, 10, "This emulator is");
  put(0, 11, "Free Open Source Software");
  put(0, 12, "released under the GPLv3.");
  put(0, 14, "If you paid extra for it you");
  put(0, 15, "probably got scammed and");
  put(0, 16, "should ask for a refund");
  put(0, 17, "immediately!");
  put(0, 19, "Source and more information:");
  put(0, 20, "github.com/bastetfurry/espnes", CYAN);
  put(0, 22, "FabGL and DBOPL used under");
  put(0, 23, "the GPL licence.");
  put(0, 24, "See the github page for details!");
  box(0, ROWS - 3, COLS, 3);
  hint(23, ROWS - 2, "A: Back");
  for (;;) {
    flush();
    MenuKey k = menuKey();
    if (k == MK_A || k == MK_B || k == MK_START || k == MK_ESC)
      return;
    if (k == MK_ROM && s_serialRom >= 1 && s_serialRom <= 3)
      k = (MenuKey)(MK_1 + s_serialRom - 1);
    if (k >= MK_1 && k <= MK_3)
      sound((ui::Kind)(k - MK_1));
  }
}


// The manual viewer: a teletext style page per screen (romindex.h). A or Right turns to the next
// page, B or Left to the one before (Up/Down and Page Up/Down too), Start or Esc goes back.
static void showManual(RomBlob const & rom)
{
  using namespace ui;
  static const uint8_t colours[8] = { BLACK, RED, GREEN, YELLOW, LINE, MAGENTA, CYAN, BRIGHT };
  // the pages, found by their form feeds
  std::vector<char const *> pages;
  pages.push_back(rom.manual);
  for (char const * p = rom.manual; *p; ++p)
    if (*p == '\f')
      pages.push_back(p + 1);

  int page = 0;
  invalidate();
  for (;;) {
    clear();
    putc(0, 0, G_V, LINE);
    putc(COLS - 1, 0, G_V, LINE);
    char text[33];
    snprintf(text, 25, "%s", rom.title);
    put(1, 0, text, GREEN);
    snprintf(text, sizeof(text), "%02d/%02d", page + 1, (int) pages.size());
    put(COLS - 1 - strlen(text), 0, text, YELLOW);
    divider(0, 1, COLS);
    putc(0, 1, G_BL, LINE);
    putc(COLS - 1, 1, G_BR, LINE);

    // the page: ESC and a digit set the colour, a line ends at '\n', the page at '\f' or the end
    char const * p = pages[page];
    uint8_t colour = BRIGHT;
    for (int row = 0; row < MANUAL_ROWS && *p && *p != '\f'; ++row) {
      for (int col = 0; *p && *p != '\n' && *p != '\f'; ++p) {
        if (*p == 0x1b && p[1]) {
          colour = colours[(p[1] - '0') & 7];
          ++p;
        } else if (col < MANUAL_COLS) {
          putc(col++, 2 + row, (uint8_t) *p, colour);
        }
      }
      if (*p == '\n')
        ++p;
    }

    divider(0, ROWS - 2, COLS);
    putc(0, ROWS - 2, G_TL, LINE);
    putc(COLS - 1, ROWS - 2, G_TR, LINE);
    putc(0, ROWS - 1, G_V, LINE);
    putc(COLS - 1, ROWS - 1, G_V, LINE);
    hint(1, ROWS - 1, "B: < A: > +: <>    Start: Back");
    flush();

    int step = 0;
    switch (menuKey()) {
      case MK_A: case MK_RIGHT: case MK_DOWN: case MK_PAGEDOWN: step = 1; break;
      case MK_B: case MK_LEFT:  case MK_UP:   case MK_PAGEUP:   step = -1; break;
      case MK_START: case MK_ESC:
        return;
      default:
        break;
    }
    page = std::max(0, std::min((int) pages.size() - 1, page + step));
  }
}


// The options over the main menu (Start). Returns true to start the selected game, from *slot
// when that is set.
enum class Opt { MANUAL, SAVE, LOAD, CHEATS, MEMORY, TV, JOYPAD, FILES, ABOUT, QUIT };

static bool optionsMenu(int selected, int top, GameFacts & facts, int * slot)
{
  static constexpr int COL = 7, ROW = 8, W = 18, H = 14;
  static const Opt items[] = { Opt::MANUAL, Opt::SAVE, Opt::LOAD, Opt::CHEATS, Opt::MEMORY,
                               Opt::TV, Opt::JOYPAD, Opt::FILES, Opt::ABOUT, Opt::QUIT };
  static constexpr int COUNT = sizeof(items) / sizeof(items[0]);
  RomBlob const & rom = g_roms[selected];
  int saveChoice = 1, loadChoice = 1;

  auto enabled = [&](Opt o) {
    switch (o) {
      case Opt::MANUAL: return rom.manual != nullptr;
      case Opt::SAVE:   return s_paused == selected && !facts.music;
      case Opt::LOAD:   return facts.slots[0] || facts.slots[1];
      case Opt::CHEATS: return !facts.music;
      case Opt::MEMORY: return false;                // to come
      case Opt::QUIT:   return s_paused >= 0;
      default:          return true;
    }
  };
  int sel = 0;
  while (!enabled(items[sel]))
    ++sel;
  auto move = [&](int step) {
    int s = sel;
    do
      s = (s + COUNT + step) % COUNT;
    while (!enabled(items[s]));
    sel = s;
  };

  for (;;) {
    // the menu under it, then the box
    drawMainMenu(selected, top, facts);
    ui::box(COL, ROW, W, H);
    for (int i = 0; i < COUNT; ++i) {
      Opt o = items[i];
      int row = ROW + 1 + i;
      bool on = i == sel, ok = enabled(o);
      uint8_t bg = on ? ui::SEL_BG : ui::BLACK, fg = on ? ui::SEL_TEXT : ok ? ui::TEXT : ui::GREY;
      static char const * const names[] = { "Manual", "Save State", "Load State", "Cheats", "Edit Memory",
                                            "TV", "Config Joypad", "Files", "About", "Quit Current" };
      ui::fill(COL + 1, row, W - 2, 1, ' ', fg, bg);
      ui::put(COL + 2, row, names[i], fg, bg);
      if (o == Opt::SAVE || o == Opt::LOAD) {
        // the slots: green with a state, grey without; the chosen one inverted
        int chosen = o == Opt::SAVE ? saveChoice : loadChoice;
        for (int s = 1; s <= 2; ++s) {
          uint8_t c = facts.slots[s - 1] ? ui::GREEN : ui::GREY;
          bool inv = on && s == chosen;
          ui::putc(COL + 11 + 2 * s, row, '0' + s, !inv ? c : c == ui::GREEN ? (uint8_t) ui::BLACK : (uint8_t) ui::BRIGHT, inv ? c : bg);
        }
      }
      if (o == Opt::TV) {
        char text[8];
        snprintf(text, sizeof(text), "[%s]", regionChoiceName());
        ui::put(COL + W - 2 - strlen(text), row, text, fg, bg);
      }
    }
    ui::divider(COL, ROW + H - 3, W);
    ui::hint(COL + 2, ROW + H - 2, "A: OK B: Back");
    if (ui::flush())                  // back from a screen of its own
      drawPicture(selected, PIC_COL * 8, PIC_ROW * 8, true);

    MenuKey key = menuKey();
    Opt o = items[sel];
    switch (key) {
      case MK_UP:   move(-1); break;
      case MK_DOWN: move(1); break;
      case MK_B: case MK_START: case MK_ESC:
        return false;
      case MK_LEFT: case MK_RIGHT:
        if (o == Opt::SAVE)
          saveChoice = 3 - saveChoice;
        else if (o == Opt::LOAD)
          loadChoice = 3 - loadChoice;
        else if (o == Opt::TV)
          s_regionChoice = (RegionChoice)(((int) s_regionChoice + (key == MK_LEFT ? 2 : 1)) % 3);
        break;
      case MK_A:
        switch (o) {
          case Opt::MANUAL:
            showManual(rom);
            ui::invalidate();
            break;
          case Opt::SAVE: {
            char question[24];
            snprintf(question, sizeof(question), "Overwrite slot %d?", saveChoice);
            if (facts.slots[saveChoice - 1] && !confirm(question))
              break;
            if (char const * err = saveSlot(s_paused, saveChoice)) {
              ui::message(ui::BAD, err);
              break;
            }
            snprintf(question, sizeof(question), "Saved in slot %d", saveChoice);
            ui::message(ui::GOOD, question);
            gatherFacts(selected, facts);
            return false;
          }
          case Opt::LOAD:
            if (!facts.slots[loadChoice - 1]) {
              ui::message(ui::BAD, loadChoice == 1 ? "Slot 1 is empty" : "Slot 2 is empty");
              break;
            }
            if (s_paused >= 0 && !confirm(s_paused == selected ? "Load over the\nrunning game?" : "Quit the\nrunning game?"))
              break;
            *slot = loadChoice;
            return true;
          case Opt::CHEATS:
            editCheats(rom);
            if (selected == s_paused)
              applyCheats(rom);
            ui::invalidate();
            gatherFacts(selected, facts);
            break;
          case Opt::TV:
            s_regionChoice = (RegionChoice)(((int) s_regionChoice + 1) % 3);
            break;
          case Opt::JOYPAD: {
            char const * msg = editPadKeys();
            ui::invalidate();
            if (msg)
              ui::message(strcmp(msg, "Keys saved") ? ui::NEUTRAL : ui::GOOD, msg);
            break;
          }
          case Opt::FILES:
            if (fileManager())
              ui::message(ui::GOOD, "Files deleted");
            ui::invalidate();
            gatherFacts(selected, facts);
            break;
          case Opt::ABOUT:
            aboutScreen();
            break;
          case Opt::QUIT:
            if (confirm("Quit the\nrunning game?")) {
              s_paused = -1;
              ui::message(ui::NEUTRAL, "Game quit");
              return false;
            }
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }
}


// Shows the list until a game is picked. Returns its index, and in *slot the savestate slot to
// start it from (0: none; for the paused game that means carrying on).
static int chooseRom(int * slot)
{
  *slot = 0;
  static int selected = 0;
  GameFacts facts;
  int shownPicture = -1;
  listRoms();
  ui::invalidate();                   // a game or something else was on the screen
  if (selected >= g_romCount)
    selected = 0;
  if (s_paused >= 0)
    selected = s_paused;
  int n = g_romCount;
  for (;;) {
    int top = std::max(0, std::min(selected - LIST_ROWS / 2, n - LIST_ROWS));
    if (n && facts.idx != selected)
      gatherFacts(selected, facts);
    drawMainMenu(selected, top, facts);
    if ((ui::flush() || shownPicture != selected) && n) {
      drawPicture(selected, PIC_COL * 8, PIC_ROW * 8, true);
      shownPicture = selected;
    }

    MenuKey key = menuKey();
    switch (key) {
      case MK_ROM:                    // over serial: start that game straight away
        if (s_serialRom >= 0 && s_serialRom < n) {
          selected = s_serialRom;
          return selected;
        }
        break;
      case MK_UP:       if (n) selected = (selected + n - 1) % n; break;
      case MK_DOWN:     if (n) selected = (selected + 1) % n; break;
      case MK_LEFT:  case MK_PAGEUP:   selected = std::max(0, selected - LIST_ROWS); break;
      case MK_RIGHT: case MK_PAGEDOWN: if (n) selected = std::min(n - 1, selected + LIST_ROWS); break;
      case MK_START:
        if (n && optionsMenu(selected, top, facts, slot))
          return selected;
        shownPicture = -1;            // the box covered it
        break;
      case MK_A:
        if (!n)
          break;
        if (s_paused >= 0 && selected != s_paused && !confirm("Quit the\nrunning game?"))
          break;
        return selected;
      default:
        break;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// System menu (see MenuMockup.txt): SysRq (Alt+PrintScreen) in every game, as in ESPPC, or A+B+
// Start+Select on pad 1. The game stands still while it is open; the box is drawn over its
// picture, the rest of the grid is left alone (ui::picture), and the next frame paints it all again.

enum class SysAction { MANUAL, SAVE, LOAD, CHEATS, MEMORY, SRAM, TAPE, INPUT, KEYMAP, VOLUME, RESET, MAINMENU };

// Returns true to go back to the main menu.
static bool systemMenu()
{
  RomBlob const & rom = *s_trapRom;
  int idx = s_trapRom - g_roms;
  bool music = nes.isNsf();
  bool tape = rom.tapeTrap || nes.famiKeyboard() || !s_tape.empty() || s_tape.mode() != Tape::STOPPED;
  SysAction items[12];
  int count = 0;
  for (SysAction a : { SysAction::MANUAL, SysAction::SAVE, SysAction::LOAD, SysAction::CHEATS, SysAction::MEMORY,
                       SysAction::SRAM, SysAction::TAPE, SysAction::INPUT, SysAction::KEYMAP, SysAction::VOLUME,
                       SysAction::RESET, SysAction::MAINMENU }) {
    if (a == SysAction::SRAM && s_savePath.isEmpty())
      continue;
    if (a == SysAction::TAPE && !tape)
      continue;
    if ((a == SysAction::INPUT || a == SysAction::KEYMAP) && !nes.famiKeyboard())
      continue;
    items[count++] = a;
  }
  bool slots[2] = { !music && slotUsed(rom, 1), !music && slotUsed(rom, 2) };
  auto enabled = [&](SysAction a) {
    switch (a) {
      case SysAction::MANUAL: return rom.manual != nullptr;
      case SysAction::SAVE:   return !music;
      case SysAction::LOAD:   return slots[0] || slots[1];
      case SysAction::CHEATS: return !music;
      case SysAction::MEMORY: return false;              // to come
      default:                return true;
    }
  };
  int sel = 0, saveChoice = 1, loadChoice = 1, tapeChoice = 0;
  auto move = [&](int step) {
    do
      sel = (sel + count + step) % count;
    while (!enabled(items[sel]));
  };
  if (!enabled(items[sel]))
    move(1);

  releaseFamiKeys();
  s_padState[0] = s_padState[1] = 0;
  menuKeyStart();
  Serial.println("System menu: u/d select, a does it, l/r change, b leaves");
  // a changed layout is kept when the menu closes
  struct KeepLayout {
    int before;
    ~KeepLayout() { if (s_layout != before) storeLayout(); }
  } keep { s_layout };

  // The game's picture stays around the box. A message covers the top row for a while: that row
  // is kept, to be put back.
  static constexpr int W = 21, COL = (ui::COLS - W) / 2;
  int h = (tape ? 3 : 1) + count + 4, row = (ui::ROWS - h) / 2;
  std::vector<uint8_t> topRow(8 * 256);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 256; ++x)
      topRow[y * 256 + x] = s_lines[y][x ^ 2];
  bool background = true;             // the game's picture is still around the box
  ui::picture(0, 0, ui::COLS, ui::ROWS);
  ui::flush();                        // what is shown: the picture everywhere
  ui::messageArea(0, ui::COLS);

  for (;;) {
    using namespace ui;
    if (background)
      picture(0, 0, COLS, ROWS);
    else
      clear();
    box(COL, row, W, h);
    int r = row + 1;
    char text[24];
    if (tape) {
      // where the tape is
      snprintf(text, sizeof(text), "Tape: %u/%u", (unsigned) s_tape.position(), (unsigned) s_tape.length());
      put(COL + 2, r, text, s_tape.mode() == Tape::PLAYING ? GREEN : s_tape.mode() == Tape::RECORDING ? RED : TEXT);
      divider(COL, r + 1, W);
      r += 2;
    }
    for (int i = 0; i < count; ++i, ++r) {
      SysAction a = items[i];
      bool on = i == sel, ok = enabled(a);
      uint8_t bg = on ? SEL_BG : BLACK, fg = on ? SEL_TEXT : ok ? TEXT : GREY;
      static char const * const names[] = { "Manual", "Save State", "Load State", "Cheats", "Edit Memory",
                                            "Force SRAM Save", "Tape", "Input:", "Keymap", "Volume",
                                            "Reset Console", "Main Menu" };
      fill(COL + 1, r, W - 2, 1, ' ', fg, bg);
      put(COL + 2, r, names[(int) a], fg, bg);
      // what is chosen on the row: inverted while it is selected
      auto choice = [&](int col, char const * t, uint8_t c, bool chosen) {
        bool inv = on && chosen;
        put(COL + col, r, t, !inv ? c : c == TEXT || c == GREEN ? (uint8_t) BLACK : (uint8_t) BRIGHT, inv ? c : bg);
      };
      switch (a) {
        case SysAction::SAVE: case SysAction::LOAD:
          for (int s = 1; s <= 2; ++s)
            choice(14 + 2 * s, s == 1 ? "1" : "2", slots[s - 1] ? GREEN : GREY,
                   s == (a == SysAction::SAVE ? saveChoice : loadChoice));
          break;
        case SysAction::TAPE: {
          static char const * const buttons[] = { "\x10", "\x07", "\xdb", "\xae" };
          Tape::Mode m = s_tape.mode();
          uint8_t colours[] = { m == Tape::PLAYING ? GREEN : TEXT, m == Tape::RECORDING ? RED : TEXT, TEXT, TEXT };
          for (int b = 0; b < 4; ++b)
            choice(12 + 2 * b, buttons[b], colours[b], b == tapeChoice);
          break;
        }
        case SysAction::INPUT:
          put(COL + 12, r, "Key", s_famiMode ? GREEN : GREY, bg);
          put(COL + 16, r, "Joy", s_famiMode ? GREY : GREEN, bg);
          break;
        case SysAction::KEYMAP: case SysAction::VOLUME:
          if (a == SysAction::KEYMAP)
            snprintf(text, sizeof(text), "[%s]", currentLayout()->name);
          else
            snprintf(text, sizeof(text), "[%d/8]", s_volume);
          put(COL + W - 2 - strlen(text), r, text, fg, bg);
          break;
        default:
          break;
      }
    }
    divider(COL, r, W);
    hint(COL + 2, r + 1, "<>^v: Crsr");
    hint(COL + 2, r + 2, "A: OK     B: Back");
    if (flush() && background) {
      // the message is gone: the top row of the game's picture back
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 256; ++x)
          if (pictureShown(x, y))
            s_lines[y][x ^ 2] = topRow[y * 256 + x];
    }

    MenuKey key = menuKey();
    SysAction a = items[sel];
    bool change = key == MK_LEFT || key == MK_RIGHT;
    int step = key == MK_LEFT ? -1 : 1;
    // after a screen of its own the menu stands on black
    auto screen = [&]() {
      ui::invalidate();
      background = false;
    };
    switch (key) {
      case MK_UP:   move(-1); continue;
      case MK_DOWN: move(1); continue;
      case MK_B: case MK_START: case MK_ESC:
        return false;
      default:
        break;
    }
    if (change) {
      switch (a) {
        case SysAction::SAVE:   saveChoice = 3 - saveChoice; break;
        case SysAction::LOAD:   loadChoice = 3 - loadChoice; break;
        case SysAction::TAPE:   tapeChoice = (tapeChoice + 4 + step) % 4; break;
        case SysAction::INPUT:  setFamiMode(!s_famiMode); break;
        case SysAction::KEYMAP: setLayout(s_layout + step); break;
        case SysAction::VOLUME:
          s_volume = std::max(0, std::min(8, s_volume + step));
          ui::sound(ui::NEUTRAL);         // to hear how loud
          break;
        default:
          break;
      }
      continue;
    }
    if (key != MK_A)
      continue;
    switch (a) {
      case SysAction::MANUAL:
        showManual(rom);
        screen();
        break;
      case SysAction::SAVE: {
        char question[24];
        snprintf(question, sizeof(question), "Overwrite slot %d?", saveChoice);
        if (slots[saveChoice - 1] && !confirm(question))
          break;
        if (char const * err = saveSlot(idx, saveChoice)) {
          ui::message(ui::BAD, err);
          break;
        }
        slots[saveChoice - 1] = true;
        snprintf(question, sizeof(question), "Saved in slot %d", saveChoice);
        ui::message(ui::GOOD, question);
        break;
      }
      case SysAction::LOAD: {
        if (!slots[loadChoice - 1]) {
          ui::message(ui::BAD, loadChoice == 1 ? "Slot 1 is empty" : "Slot 2 is empty");
          break;
        }
        if (!confirm("Load over the\nrunning game?"))
          break;
        if (char const * err = loadSlot(rom, loadChoice)) {
          ui::message(ui::BAD, err);
          break;
        }
        showOverlay(loadChoice == 1 ? "SLOT 1 LOADED" : "SLOT 2 LOADED");
        return false;
      }
      case SysAction::CHEATS:
        editCheats(rom);
        applyCheats(rom);
        screen();
        break;
      case SysAction::SRAM: {
        int done = storeSave();
        ui::message(done > 0 ? ui::GOOD : done == 0 ? ui::NEUTRAL : ui::BAD,
                    done > 0 ? "SRAM saved" : done == 0 ? "SRAM unchanged" : "Saving the SRAM failed");
        break;
      }
      case SysAction::TAPE:
        // play and record go back to the game, stop and rewind stay
        switch (tapeChoice) {
          case 0: tapePlay(); return false;
          case 1: tapeRecord(); return false;
          case 2: tapeStop(); break;
          case 3: s_tape.rewind(); showTapeState("REWOUND"); break;
        }
        break;
      case SysAction::INPUT:
        setFamiMode(!s_famiMode);
        break;
      case SysAction::KEYMAP:
        setLayout(s_layout + 1);
        break;
      case SysAction::RESET:
        if (!confirm("Reset the\nconsole?"))
          break;
        resetConsole();
        Serial.println("Reset");
        return false;
      case SysAction::MAINMENU:
        return true;
      default:
        break;
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
  s_canvas->selectFont(&FONT_8x8);
  s_canvas->setGlyphOptions(GlyphOptions().FillBackground(true));

  // for save games later; an unformatted partition is formatted on the first boot
  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS could not be mounted");
  loadLayout();
  loadPadKeys();
  initPads();
  nes.setTape(&s_tape);

  Serial.printf("Free heap: %u bytes, largest block %u\n",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  for (;;) {
    int slot;
    int idx = chooseRom(&slot);
    if (idx != s_paused || slot) {
      // a new game: the paused one ends
      s_paused = -1;
      char const * err = loadRom(g_roms[idx]);
      if (!err && slot)
        err = loadSlot(g_roms[idx], slot);
      if (err) {
        ui::message(ui::BAD, err);
        continue;
      }
    }
    s_canvas->waitCompletion();
    runGame();
    storeSave();
    // the console waits as it is, unless the game crashed it
    s_paused = nes.cpu.jammed ? -1 : idx;
    if (nes.cpu.jammed)
      ui::message(ui::BAD, "The game crashed the CPU");
  }
}


// the IDF's entry point, in the main task on core 1 (sdkconfig.defaults)
extern "C" void app_main()
{
  setup();
}
