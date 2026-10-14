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
// Keys: arrows = D-pad, Ctrl = A, Alt = B, Shift = Select, Space = Start,
//       Esc = back to the ROM list, F1 = reset, F2/F3 = volume down/up.
// Serial console: a number starts that ROM from the list, 'q' leaves the game, and
// a b e s u d l r tap A, B, sElect, Start and the directions.

#include <Arduino.h>
#include <SPIFFS.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "fabgl.h"
#include "esp_timer.h"
#include "esp32/rom/crc.h"

#include "nes/nes.h"
#include "nes/palette.h"
#include "romindex.h"


static fabgl::VGAController   VGA;
static fabgl::PS2Controller   PS2;
static Nes                    nes;

static uint8_t                s_raw[64];        // NES colour -> raw VGA pixel (RGB222 + sync bits)
static uint8_t *              s_lines[240];     // the frame buffer's line pointers


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
    Serial.printf("  %d: %s (%u bytes)\n", i, g_roms[i].name, (unsigned) g_roms[i].size);
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

  if (!g_romCount) {
    cv.setPenColor(Color::BrightRed);
    cv.drawText(8, 40, "No ROMs built in.");
    cv.setPenColor(Color::White);
    cv.drawText(8, 56, "Put .nes files in roms/ and");
    cv.drawText(8, 68, "run pio run -t upload");
  }
  for (int i = 0; i < visible && top + i < g_romCount; ++i) {
    int idx = top + i;
    int y = 20 + i * 9;
    if (idx == selected) {
      cv.setBrushColor(Color::Blue);
      cv.fillRectangle(0, y - 1, 255, y + 7);
    }
    String n = g_roms[idx].name;
    if (n.endsWith(".nes") || n.endsWith(".NES"))
      n = n.substring(0, n.length() - 4);
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
  cv.drawText(4, 231, "Esc=list F1=reset F2/F3=vol");
  cv.waitCompletion();
}


// Shows the list until a ROM is picked. Returns its index.
static int chooseRom(char const * message)
{
  static int selected = 0;
  const int visible = 20;
  auto kb = PS2.keyboard();
  listRoms();
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

// Settings from a ROM's optional .cfg (built in next to it, see tools/embed_roms.py):
//   save   = yes | no      battery backed save RAM, overriding the header's battery flag
//   region = ntsc | pal    TV system, overriding the header and the file name
// One "key = value" per line; '#' starts a comment.
struct RomConfig {
  int save   = -1;          // -1 = from the header
  int region = -1;          // -1 = automatic, else (int) Region
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
// its contents, so a renamed ROM keeps its save
static String savePathFor(RomBlob const & rom)
{
  String stem;
  for (char const * p = rom.name; *p && stem.length() < 14; ++p) {
    char c = *p;
    if (c == '.' && !strcasecmp(p, ".nes"))
      break;
    stem += isalnum((unsigned char) c) ? c : '_';
  }
  char crc[10];
  snprintf(crc, sizeof(crc), "_%08x", (unsigned) crc32_le(0, rom.data, rom.size));
  return "/" + stem + crc + ".sav";
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

  nes.powerOn();
  Serial.printf("Loaded %s: mapper %d, PRG %d KB, CHR %d KB%s, %s%s, free heap %u\n",
                rom.name, nes.cart().info.mapper, nes.cart().info.prgSize / 1024,
                nes.cart().info.chrSize / 1024, nes.cart().info.chrSize ? "" : " (RAM)",
                nes.region() == Region::PAL ? "PAL" : "NTSC", battery ? ", battery save" : "",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
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
      if (!down)
        continue;
      if (vk == fabgl::VK_ESCAPE)
        return;
      if (vk == fabgl::VK_F1)
        nes.powerOn();
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
      static char const keys[] = "abesudlr";
      if (char const * k = strchr(keys, c)) {
        serialButtons = 1 << (k - keys);
        serialFrames = 10;
      }
    }
    nes.setButtons(0, readPad(kb) | (serialFrames ? serialButtons : 0));
    if (serialFrames)
      --serialFrames;

    int64_t t0 = esp_timer_get_time();
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

  Serial.printf("Free heap: %u bytes, largest block %u\n",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  for (;;) {
    int idx = chooseRom(message);
    message = loadRom(g_roms[idx]);
    if (message)
      continue;
    s_canvas->waitCompletion();
    runGame();
    storeSave();
  }
}


void loop()
{
}
