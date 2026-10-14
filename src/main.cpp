// ESPNES - NES emulator on an ESP32 with FabGL VGA output and a PS/2 keyboard as the pad.
//
// Video: 320x240 at 60 Hz with the NES's 256x240 picture centred (black side borders). FabGL's
// VGAController sends the frame buffer by DMA straight from its line buffers, and the PPU writes
// its finished lines into those buffers as raw VGA pixels, so the picture costs no interrupt time.
//
// Keys: arrows = D-pad, Ctrl = A, Alt = B, Shift = Select, Space = Start,
//       Esc = back to the ROM list, F1 = reset.
// Serial console: a number starts that ROM from the list, 'q' leaves the game.

#include <Arduino.h>
#include <SPIFFS.h>
#include <vector>
#include <algorithm>

#include "fabgl.h"
#include "esp_timer.h"

#include "nes/nes.h"
#include "nes/palette.h"


static fabgl::VGAController   VGA;
static fabgl::PS2Controller   PS2;
static Nes                    nes;

static uint8_t                s_raw[64];        // NES colour -> raw VGA pixel (RGB222 + sync bits)
static uint32_t *             s_lines[240];     // the frame buffer's line pointers


// Converts a line of NES colours to raw pixels. In a 32 bit word the VGA DMA sends byte 2 first,
// then 3, 0, 1 (see VGABaseController::getScanline), so pixels are stored in that order.
static void NES_HOT lineSink(void *, int y, uint8_t const * c)
{
  uint32_t * dst = s_lines[y];
  for (int x = 0; x < 256; x += 4, c += 4)
    *dst++ = s_raw[c[2]] | (s_raw[c[3]] << 8) | (s_raw[c[0]] << 16) | (s_raw[c[1]] << 24);
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
    s_lines[y] = (uint32_t *) VGA.getScanline(y);
  nes.setLineSink(lineSink, nullptr);
}


///////////////////////////////////////////////////////////////////////////////
// ROM list

enum class RegionChoice { Auto, NTSC, PAL };

struct RomEntry {
  String name;     // without the leading slash
  size_t size;
};

static std::vector<RomEntry> s_roms;
static RegionChoice          s_regionChoice = RegionChoice::Auto;


static void scanRoms()
{
  s_roms.clear();
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    if (name.startsWith("/"))
      name = name.substring(1);
    String lower = name;
    lower.toLowerCase();
    if (lower.endsWith(".nes"))
      s_roms.push_back({ name, f.size() });
  }
  std::sort(s_roms.begin(), s_roms.end(), [](RomEntry const & a, RomEntry const & b) {
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  Serial.println("ROMs (send the number over serial to start one):");
  for (int i = 0; i < (int)s_roms.size(); ++i)
    Serial.printf("  %d: %s (%u bytes)\n", i, s_roms[i].name.c_str(), (unsigned) s_roms[i].size);
}


static fabgl::Canvas * s_canvas;

static void drawMenu(int selected, int top, int visible, char const * message)
{
  fabgl::Canvas & cv = *s_canvas;
  cv.setBrushColor(Color::Black);
  cv.clear();
  cv.setPenColor(Color::BrightYellow);
  cv.drawText(8, 4, "ESPNES");
  cv.setPenColor(Color::White);
  static char const * regionNames[] = { "Auto", "NTSC", "PAL" };
  char buf[48];
  snprintf(buf, sizeof(buf), "TV: %s", regionNames[(int)s_regionChoice]);
  cv.drawText(256 - 8 - strlen(buf) * 8, 4, buf);

  if (s_roms.empty()) {
    cv.setPenColor(Color::BrightRed);
    cv.drawText(8, 40, "No .nes files in SPIFFS.");
    cv.setPenColor(Color::White);
    cv.drawText(8, 56, "Put ROMs in roms/ and run");
    cv.drawText(8, 68, "pio run -t uploadfs");
  }
  for (int i = 0; i < visible && top + i < (int)s_roms.size(); ++i) {
    int idx = top + i;
    int y = 20 + i * 9;
    if (idx == selected) {
      cv.setBrushColor(Color::Blue);
      cv.fillRectangle(0, y - 1, 255, y + 7);
    }
    String n = s_roms[idx].name;
    if (n.length() > 31)
      n = n.substring(0, 31);
    cv.setPenColor(idx == selected ? Color::BrightWhite : Color::White);
    cv.drawText(4, y, n.c_str());
    cv.setBrushColor(Color::Black);
  }

  if (message) {
    cv.setPenColor(Color::BrightRed);
    cv.drawText(8, 210, message);
  }
  cv.setPenColor(Color::BrightBlack);
  cv.drawText(4, 222, "Up/Down Enter:start  L/R:TV");
  cv.drawText(4, 231, "In game: Esc=list F1=reset");
  cv.waitCompletion();
}


// Shows the list until a ROM is picked. Returns its index.
static int chooseRom(char const * message)
{
  static int selected = 0;
  const int visible = 20;
  auto kb = PS2.keyboard();
  scanRoms();
  if (selected >= (int)s_roms.size())
    selected = 0;
  for (;;) {
    int top = std::max(0, std::min(selected - visible / 2, (int)s_roms.size() - visible));
    drawMenu(selected, top, visible, message);
    // a number on the serial console starts that ROM, for testing without a keyboard
    int n = s_roms.size();
    bool down = false;
    fabgl::VirtualKey vk = fabgl::VK_NONE;
    while (vk == fabgl::VK_NONE) {
      if (Serial.available()) {
        int idx = Serial.parseInt();
        if (idx >= 0 && idx < n)
          return selected = idx;
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
// running a game

static uint8_t * s_romImage = nullptr;


static Region guessRegion(String const & name)
{
  if (s_regionChoice == RegionChoice::NTSC)
    return Region::NTSC;
  if (s_regionChoice == RegionChoice::PAL)
    return Region::PAL;
  if (nes.cart().info.regionKnown)
    return nes.cart().info.region;
  // GoodNES / No-Intro naming
  for (char const * tag : { "(E)", "(Europe)", "(PAL)", "(EU)", "(G)", "(Germany)" })
    if (name.indexOf(tag) >= 0)
      return Region::PAL;
  return Region::NTSC;
}


// Returns nullptr or an error message.
static char const * loadRom(RomEntry const & rom)
{
  nes.eject();
  free(s_romImage);
  s_romImage = nullptr;

  File f = SPIFFS.open("/" + rom.name, "r");
  if (!f)
    return "Cannot open the file";
  s_romImage = (uint8_t *) malloc(rom.size);
  if (!s_romImage)
    return "ROM too big for the free RAM";
  size_t got = f.read(s_romImage, rom.size);
  f.close();
  if (got != rom.size)
    return "Read error";

  if (char const * err = nes.insert(s_romImage, rom.size))
    return err;
  nes.setRegion(guessRegion(rom.name));
  nes.powerOn();
  Serial.printf("Loaded %s: mapper %d, PRG %d KB, CHR %d KB%s, %s, free heap %u\n",
                rom.name.c_str(), nes.cart().info.mapper, nes.cart().info.prgSize / 1024,
                nes.cart().info.chrSize / 1024, nes.cart().info.chrSize ? "" : " (RAM)",
                nes.region() == Region::PAL ? "PAL" : "NTSC", (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
  return nullptr;
}


static uint8_t readPad(fabgl::Keyboard * kb)
{
  uint8_t b = 0;
  if (kb->isVKDown(fabgl::VK_UP)    || kb->isVKDown(fabgl::VK_KP_UP))    b |= BTN_UP;
  if (kb->isVKDown(fabgl::VK_DOWN)  || kb->isVKDown(fabgl::VK_KP_DOWN))  b |= BTN_DOWN;
  if (kb->isVKDown(fabgl::VK_LEFT)  || kb->isVKDown(fabgl::VK_KP_LEFT))  b |= BTN_LEFT;
  if (kb->isVKDown(fabgl::VK_RIGHT) || kb->isVKDown(fabgl::VK_KP_RIGHT)) b |= BTN_RIGHT;
  if (kb->isVKDown(fabgl::VK_LCTRL) || kb->isVKDown(fabgl::VK_RCTRL))    b |= BTN_A;
  if (kb->isVKDown(fabgl::VK_LALT)  || kb->isVKDown(fabgl::VK_RALT))     b |= BTN_B;
  if (kb->isVKDown(fabgl::VK_LSHIFT) || kb->isVKDown(fabgl::VK_RSHIFT))  b |= BTN_SELECT;
  if (kb->isVKDown(fabgl::VK_SPACE))                                     b |= BTN_START;
  // a real pad cannot press opposite directions; some games crash when it happens
  if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN))       b &= ~(BTN_UP | BTN_DOWN);
  if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= ~(BTN_LEFT | BTN_RIGHT);
  return b;
}


// Runs the loaded game until Esc.
static void runGame()
{
  auto kb = PS2.keyboard();
  kb->emptyVirtualKeyQueue();

  // NTSC 60.0988 Hz, PAL 50.007 Hz
  const int64_t period = nes.region() == Region::PAL ? 19997 : 16639;
  const int maxSkip = 3;

  int64_t next = esp_timer_get_time();
  int skipped = 0;
  bool draw = true;

  // statistics, printed every 5 seconds
  int64_t statStart = next, busy = 0, worst = 0;
  int frames = 0, drawn = 0;

  for (;;) {
    // keys other than the pad
    while (kb->virtualKeyAvailable()) {
      bool down;
      auto vk = kb->getNextVirtualKey(&down, 0);
      if (!down)
        continue;
      if (vk == fabgl::VK_ESCAPE)
        return;
      if (vk == fabgl::VK_F1)
        nes.powerOn();
    }
    if (Serial.available() && Serial.read() == 'q')
      return;
    nes.setButtons(0, readPad(kb));

    int64_t t0 = esp_timer_get_time();
    nes.runFrame(draw);
    int64_t t1 = esp_timer_get_time();
    busy += t1 - t0;
    worst = std::max(worst, t1 - t0);
    ++frames;
    drawn += draw;

    if (nes.cpu.jammed) {
      Serial.printf("CPU jammed at $%04X\n", nes.cpu.PC);
      return;
    }

    // pacing: wait for the frame's time slot; when behind, skip drawing to catch up
    next += period;
    int64_t now = esp_timer_get_time();
    if (now < next) {
      int64_t wait = next - now;
      if (wait > 2000)
        vTaskDelay(pdMS_TO_TICKS((wait - 1000) / 1000));
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
      statStart = now;
      busy = worst = 0;
      frames = drawn = 0;
    }
  }
}


///////////////////////////////////////////////////////////////////////////////

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESPNES");

  PS2.begin(PS2Preset::KeyboardPort0, KbdMode::CreateVirtualKeysQueue);
  initVideo();
  s_canvas = new fabgl::Canvas(&VGA);
  s_canvas->selectFont(&fabgl::FONT_8x8);
  s_canvas->setGlyphOptions(GlyphOptions().FillBackground(true));

  char const * message = nullptr;
  if (!SPIFFS.begin(false))
    message = "SPIFFS not mounted (uploadfs?)";

  Serial.printf("Free heap: %u bytes, largest block %u\n",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  for (;;) {
    int idx = chooseRom(message);
    message = loadRom(s_roms[idx]);
    if (message)
      continue;
    s_canvas->waitCompletion();
    runGame();
  }
}


void loop()
{
}
