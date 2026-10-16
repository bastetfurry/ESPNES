// PC test harness for the emulator core (src/nes). Not part of the ESP32 build.
//
//   nestest <nestest.nes> <nestest.log>   run nestest's automated mode, compare with the golden log
//   run <rom.nes> <frames> <out.ppm> [pal] [async] [wav=out.wav] [hold=BUTTONS@FROM-TO ...]
//                                         run frames and write the last one as PPM; BUTTONS is
//                                         a string of A B s(elect) S(tart) U D L R; async draws
//                                         on a second thread, as the ESP32 build does on core 0;
//                                         wav writes the sound (22050 Hz mono 16 bit)
//                                         trace=FROM-TO logs PPU register writes (and $4015 reads) in
//                                         those frames, traceapu adds the APU register writes;
//                                         sram=file loads the save RAM before and stores it after;
//                                         oc=PERCENT overclocks the CPU (0-100); cheat=CODE adds a Game
//                                         Genie or Action Replay code (repeatable); ntdump prints both
//                                         nametables' tile numbers as text at the end; ram=A,B,..
//                                         prints those CPU RAM bytes (hex addresses) at the end;
//                                         watch=A,B,.. prints those bytes whenever they change;
//                                         mic=FROM-TO holds the Famicom microphone in those frames;
//                                         statetest=F saves a state at frame F, then loads it at the
//                                         end and runs the frames from F again: they must match;
//                                         taperec=FILE@F starts the Data Recorder recording at
//                                         frame F and writes the tape to FILE at the end;
//                                         tapeplay=FILE@F plays the tape FILE from frame F;
//                                         type=F:TEXT plugs in the Family BASIC keyboard and types
//                                         TEXT from frame F (\n is RETURN; 4 frames per key down,
//                                         4 up; can be repeated)
//                                         trap=SPEC (from tools/tapetrap.py) traps the game's
//                                         tape save/load: the data goes to/comes from trapfile=FILE
//   bench <rom.nes> <frames>              time frames with drawing, for a rough relative speed

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

#include "../src/nes/nes.h"
#include "../src/nes/palette.h"


static std::vector<uint8_t> loadFile(char const * path)
{
  std::vector<uint8_t> data;
  FILE * f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    data.insert(data.end(), buf, buf + n);
  fclose(f);
  return data;
}


static int nestest(char const * romPath, char const * logPath)
{
  auto rom = loadFile(romPath);
  Nes nes;
  if (char const * err = nes.insert(rom.data(), rom.size())) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  nes.powerOn();
  nes.cpu.PC = 0xc000;      // automated mode entry
  nes.cpu.cycles = 7;

  FILE * log = fopen(logPath, "r");
  if (!log) {
    fprintf(stderr, "cannot open %s\n", logPath);
    return 1;
  }
  char line[256];
  int lineNo = 0;
  while (fgets(line, sizeof(line), log)) {
    ++lineNo;
    unsigned pc, a, x, y, p, sp;
    long cyc;
    sscanf(line, "%4x", &pc);
    sscanf(strstr(line, "A:"), "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &sp);
    sscanf(strstr(line, "CYC:"), "CYC:%ld", &cyc);
    Cpu & c = nes.cpu;
    if (c.PC != pc || c.A != a || c.X != x || c.Y != y || c.getP() != p || c.S != sp || c.cycles != cyc) {
      printf("MISMATCH at log line %d:\n  expected %s", lineNo, line);
      printf("  got      %04X  A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%d\n",
             c.PC, c.A, c.X, c.Y, c.getP(), c.S, c.cycles);
      return 1;
    }
    c.step();
  }
  // nestest leaves its result codes in $02 (official opcodes) and $03 (unofficial)
  printf("nestest: %d lines match, result $02=%02X $03=%02X\n", lineNo, nes.ram[2], nes.ram[3]);
  return (nes.ram[2] || nes.ram[3]) ? 1 : 0;
}


// the picture as NES colour indices: an identity colour map, plain byte order
static uint8_t s_frame[240][256] __attribute__((aligned(4)));
static uint8_t * s_frameLines[240];
static uint8_t s_identity[64];

static void setupVideo(Nes & nes)
{
  for (int i = 0; i < 64; ++i)
    s_identity[i] = i;
  for (int y = 0; y < 240; ++y)
    s_frameLines[y] = s_frame[y];
  nes.setVideoOutput(s_frameLines, s_identity, false);
}


static void writeWav(char const * path, std::vector<int16_t> const & samples, int rate)
{
  FILE * f = fopen(path, "wb");
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  uint32_t bytes = samples.size() * 2;
  fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  fwrite("data", 1, 4, f); u32(bytes);
  fwrite(samples.data(), 2, samples.size(), f);
  fclose(f);
}


static int s_traceFrom = -1, s_traceTo = -1;

static bool s_traceApu = false;

static void traceRead(Nes & nes, uint16_t addr, uint8_t value)
{
  if ((int) nes.frameCount < s_traceFrom || (int) nes.frameCount > s_traceTo)
    return;
  int dot, line = nes.currentLine(&dot);
  printf("frame %4u line %3d dot %3d  read $%04X -> $%02X  (cycle %d)\n", nes.frameCount, line, dot, addr, value, nes.cpu.cycles);
}

static void traceWrite(Nes & nes, uint16_t addr, uint8_t value)
{
  if ((int) nes.frameCount < s_traceFrom || (int) nes.frameCount > s_traceTo)
    return;
  if (addr >= 0x4000 && addr < 0x5000 && addr != 0x4014 && !s_traceApu)
    return;   // the PPU, OAM DMA and mapper registers at $5000-$5FFF only, unless traceapu
  int dot, line = nes.currentLine(&dot);
  printf("frame %4u line %3d dot %3d  $%04X = $%02X\n", nes.frameCount, line, dot,
         addr >= 0x2000 && addr < 0x4000 ? 0x2000 | (addr & 7) : addr, value);
}


struct Hold { uint8_t buttons; int from, to; };

static std::atomic<int> s_kicks(0);

static void kick(void *)
{
  ++s_kicks;
}

static uint8_t parseButtons(char const * s)
{
  uint8_t b = 0;
  for (; *s && *s != '@'; ++s)
    switch (*s) {
      case 'A': b |= BTN_A; break;
      case 'B': b |= BTN_B; break;
      case 's': b |= BTN_SELECT; break;
      case 'S': b |= BTN_START; break;
      case 'U': b |= BTN_UP; break;
      case 'D': b |= BTN_DOWN; break;
      case 'L': b |= BTN_LEFT; break;
      case 'R': b |= BTN_RIGHT; break;
    }
  return b;
}


static int run(int argc, char ** argv)
{
  auto rom = loadFile(argv[2]);
  int frames = atoi(argv[3]);
  char const * outPath = argv[4];
  Nes nes;
  if (char const * err = nes.insert(rom.data(), rom.size())) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  std::vector<Hold> holds;
  int micFrom = -1, micTo = -2;
  int stateAt = -1;
  bool async = false;
  char const * wavPath = nullptr;
  char const * sramPath = nullptr;
  bool ntdump = false;
  Cheat cheats[Nes::MAX_CHEATS];
  int cheatCount = 0;
  std::vector<int> peeks, watches;
  std::vector<int16_t> audio;
  Tape tape;
  char const * tapeRecPath = nullptr;
  int tapeRecAt = -1, tapePlayAt = -1;
  unsigned trapSave = 0, trapLoad = 0;
  std::vector<std::pair<int, int>> trapBlocks;
  char const * trapPath = nullptr;
  std::vector<std::pair<int, std::string>> typing;    // from frame, text
  for (int i = 5; i < argc; ++i) {
    if (!strcmp(argv[i], "pal"))
      nes.setRegion(Region::PAL);
    else if (!strcmp(argv[i], "async"))
      async = true;
    else if (!strncmp(argv[i], "wav=", 4))
      wavPath = argv[i] + 4;
    else if (!strncmp(argv[i], "ram=", 4) || !strncmp(argv[i], "watch=", 6)) {
      auto & list = argv[i][0] == 'r' ? peeks : watches;
      for (char const * p = strchr(argv[i], '=') + 1; *p; ) {
        list.push_back(strtol(p, (char **) &p, 16));
        if (*p == ',')
          ++p;
      }
    }
    else if (!strcmp(argv[i], "ntdump"))
      ntdump = true;
    else if (!strncmp(argv[i], "cheat=", 6)) {
      if (!decodeCheat(argv[i] + 6, &cheats[cheatCount]))
        fprintf(stderr, "invalid cheat %s\n", argv[i] + 6);
      else
        ++cheatCount;
    }
    else if (!strncmp(argv[i], "oc=", 3))
      nes.setOverclock(atoi(argv[i] + 3));
    else if (!strncmp(argv[i], "sram=", 5))
      sramPath = argv[i] + 5;
    else if (!strncmp(argv[i], "trace=", 6)) {
      sscanf(argv[i] + 6, "%d-%d", &s_traceFrom, &s_traceTo);
      nesTraceWrite = traceWrite;
      nesTraceRead = traceRead;
    }
    else if (!strcmp(argv[i], "traceapu"))
      s_traceApu = true;
    else if (!strncmp(argv[i], "statetest=", 10))
      stateAt = atoi(argv[i] + 10);
    else if (!strncmp(argv[i], "taperec=", 8) || !strncmp(argv[i], "tapeplay=", 9)) {
      char * at = strrchr(argv[i], '@');
      if (!at) {
        fprintf(stderr, "%s: FILE@FRAME expected\n", argv[i]);
        return 1;
      }
      *at = 0;
      char const * path = strchr(argv[i], '=') + 1;
      if (argv[i][4] == 'r') {
        tapeRecPath = path;
        tapeRecAt = atoi(at + 1);
      } else {
        auto image = loadFile(path);
        if (char const * err = tape.load(image.data(), image.size())) {
          fprintf(stderr, "%s: %s\n", path, err);
          return 1;
        }
        tapePlayAt = atoi(at + 1);
      }
      nes.setTape(&tape);
    }
    else if (!strncmp(argv[i], "trap=", 5)) {
      // save,load;addr:size,...;addr:value:original,...
      char const * p = argv[i] + 5;
      int n = 0;
      sscanf(p, "%x,%x%n", &trapSave, &trapLoad, &n);
      p += n;
      std::vector<Cheat> patches;
      for (int part = 0; *p == ';'; ++part) {
        ++p;
        while (*p && *p != ';') {
          unsigned a, b, c = 0;
          if (part == 0 && sscanf(p, "%x:%x%n", &a, &b, &n) == 2)
            trapBlocks.push_back({ (int) a, (int) b });
          else if (part == 1 && sscanf(p, "%x:%x:%x%n", &a, &b, &c, &n) == 3)
            patches.push_back({ true, (uint16_t) a, (uint8_t) b, (int16_t) c });
          else
            break;
          p += n;
          if (*p == ',')
            ++p;
        }
      }
      nes.setRomPatches(patches.data(), patches.size());
      nes.enableVcCall(true);
    }
    else if (!strncmp(argv[i], "type=", 5)) {
      char * colon = strchr(argv[i], ':');
      std::string text;
      for (char const * p = colon ? colon + 1 : ""; *p; ++p) {
        if (p[0] == '\\' && p[1] == 'n') {
          text += '\n';
          ++p;
        } else {
          text += *p;
        }
      }
      typing.push_back({ atoi(argv[i] + 5), text });
      nes.setFamiKeyboard(true);
    }
    else if (!strncmp(argv[i], "trapfile=", 9))
      trapPath = argv[i] + 9;
    else if (!strncmp(argv[i], "mic=", 4))
      sscanf(argv[i] + 4, "%d-%d", &micFrom, &micTo);
    else if (!strncmp(argv[i], "hold=", 5)) {
      Hold h;
      h.buttons = parseButtons(argv[i] + 5);
      sscanf(strchr(argv[i], '@'), "@%d-%d", &h.from, &h.to);
      holds.push_back(h);
    }
  }
  setupVideo(nes);
  std::atomic<bool> running(true);
  std::thread worker;
  if (async) {
    nes.setRenderKick(kick, nullptr);
    worker = std::thread([&] {
      int done = 0;
      while (running)
        if (s_kicks > done) {
          nes.drawQueuedLines();
          ++done;
        }
    });
  }
  if (sramPath)
    if (FILE * sf = fopen(sramPath, "rb")) {
      printf("loaded save RAM: %zu bytes\n", fread(nes.prgRam, 1, Nes::PRG_RAM_SIZE, sf));
      fclose(sf);
    }
  nes.powerOn();
  if (cheatCount) {
    nes.setCheats(cheats, cheatCount);
    nes.enableCheats(true);
  }
  struct MemorySink : StateSink {
    std::vector<uint8_t> data;
    bool write(uint8_t const * p, size_t n) override { data.insert(data.end(), p, p + n); return true; }
  } saved;
  auto inputs = [&](int f) {
    uint8_t b = 0;
    for (auto & h : holds)
      if (f >= h.from && f <= h.to)
        b |= h.buttons;
    nes.setButtons(0, b);
    nes.setMic(f >= micFrom && f <= micTo);
    if (!typing.empty()) {
      nes.releaseFamiKeys();
      for (auto & t : typing) {
        int k = (f - t.first) / 8, phase = (f - t.first) % 8;
        if (f < t.first || k >= (int) t.second.size() || phase >= 4)
          continue;
        bool shift;
        FamiKey key = famiKeyOfChar(t.second[k], &shift);
        nes.setFamiKey(key, true);
        if (shift)
          nes.setFamiKey(FK_LSHIFT, true);
      }
    }
  };
  for (int f = 0; f < frames; ++f) {
    if (f == stateAt) {
      if (!nes.saveState(saved)) {
        printf("saveState failed\n");
        return 1;
      }
      printf("state at frame %d: %zu bytes (limit %zu)\n", f, saved.data.size(), nes.stateSizeLimit());
    }
    if (f == tapeRecAt) {
      nes.tapeRecord(256 * 1024);
      printf("tape: recording from frame %d\n", f);
    }
    if (f == tapePlayAt) {
      nes.tapePlay();
      printf("tape: playing from frame %d\n", f);
    }
    Tape::Mode tapeMode = tape.mode();
    inputs(f);
    nes.runFrame(true);
    if (tapeMode == Tape::PLAYING && tape.mode() != Tape::PLAYING)
      printf("tape: ended in frame %d\n", f);
    if (uint16_t call = nes.vcCall()) {
      auto mem = [&](int a) -> uint8_t & { return a < 0x2000 ? nes.ram[a & 0x7ff] : nes.prgRam[a & 0x1fff]; };
      bool save = call == trapSave;
      printf("trap: %s in frame %d\n", save ? "save" : call == trapLoad ? "load" : "unknown call", f);
      if (FILE * tf = trapPath && (save || call == trapLoad) ? fopen(trapPath, save ? "wb" : "rb") : nullptr) {
        for (auto & b : trapBlocks)
          for (int k = 0; k < b.second; ++k) {
            if (save)
              fputc(mem(b.first + k), tf);
            else
              mem(b.first + k) = fgetc(tf);
          }
        fclose(tf);
      }
      nes.finishVcCall();
    }
    if (!watches.empty()) {
      static std::vector<int> last;
      std::vector<int> now;
      for (int a : watches)
        now.push_back(nes.read(a));
      if (now != last) {
        printf("frame %5d:", f);
        for (size_t k = 0; k < watches.size(); ++k)
          printf(" $%04X=$%02X", watches[k], now[k]);
        printf("\n");
        last = now;
      }
    }
    audio.insert(audio.end(), nes.apu.samples(), nes.apu.samples() + nes.apu.sampleCount());
    nes.apu.takeSamples();
    if (nes.cpu.jammed) {
      printf("CPU jammed at $%04X in frame %d\n", nes.cpu.PC, f);
      break;
    }
  }
  if (stateAt >= 0 && stateAt < frames && !async) {
    // the same frames again from the state: they must end the same
    static uint8_t frameA[240][256];
    memcpy(frameA, s_frame, sizeof(frameA));
    std::vector<uint8_t> ramA(nes.ram, nes.ram + 2048), prgA(nes.prgRam, nes.prgRam + Nes::PRG_RAM_SIZE);
    uint32_t cyclesA = nes.cpu.cycles;
    uint16_t pcA = nes.cpu.PC;
    memset(nes.ram, 0x55, 2048);
    memset(s_frame, 0, sizeof(s_frame));
    struct MemorySource : StateSource {
      std::vector<uint8_t> const * data; size_t pos = 0;
      size_t read(uint8_t * p, size_t n) override {
        n = std::min(n, data->size() - pos); memcpy(p, data->data() + pos, n); pos += n; return n;
      }
    } src;
    src.data = &saved.data;
    if (char const * err = nes.loadState(src)) {
      printf("loadState: %s\n", err);
      return 1;
    }
    for (int f = stateAt; f < frames; ++f) {
      inputs(f);
      nes.runFrame(true);
    }
    bool same = !memcmp(frameA, s_frame, sizeof(frameA)) && !memcmp(ramA.data(), nes.ram, 2048) &&
                !memcmp(prgA.data(), nes.prgRam, Nes::PRG_RAM_SIZE) && cyclesA == (uint32_t) nes.cpu.cycles && pcA == nes.cpu.PC;
    printf("state test: %s\n", same ? "same frame, RAM and CPU after reloading" : "DIFFERENT after reloading");
  }
  if (tapeRecPath) {
    bool overflow = tape.mode() == Tape::STOPPED && tape.overflowed();
    tape.stop();
    FILE * tf = fopen(tapeRecPath, "wb");
    fwrite(tape.data(), 1, tape.size(), tf);
    fclose(tf);
    printf("tape: %zu bytes, %u s%s\n", tape.size(), tape.playedSeconds(), overflow ? ", OVERFLOWED" : "");
  }
  running = false;
  if (worker.joinable())
    worker.join();
  if (wavPath)
    writeWav(wavPath, audio, 22050);
  if (sramPath) {
    FILE * sf = fopen(sramPath, "wb");
    fwrite(nes.prgRam, 1, Nes::PRG_RAM_SIZE, sf);
    fclose(sf);
  }
  FILE * f = fopen(outPath, "wb");
  fprintf(f, "P6\n256 240\n255\n");
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 256; ++x) {
      uint32_t c = NES_PALETTE_RGB[s_frame[y][x] & 0x3f];
      uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t) c };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  printf("%d frames, PC=$%04X\n", frames, nes.cpu.PC);
  for (int a : peeks)
    printf("$%04X = $%02X (%d)\n", a, nes.read(a), nes.read(a));
  if (ntdump) {
    // tiles as characters, assuming the game's font sits at its ASCII codes (most test ROMs)
    for (int nt = 0; nt < 2; ++nt) {
      printf("--- nametable %d\n", nt);
      for (int row = 0; row < 30; ++row) {
        for (int col = 0; col < 32; ++col) {
          uint8_t t = nes.ppu.vram[nt * 0x400 + row * 32 + col];
          putchar(t >= 0x20 && t < 0x7f ? t : '.');
        }
        putchar('\n');
      }
    }
  }
  return 0;
}


static int bench(char const * romPath, int frames)
{
  auto rom = loadFile(romPath);
  Nes nes;
  if (char const * err = nes.insert(rom.data(), rom.size())) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  setupVideo(nes);
  nes.powerOn();
  auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; ++f)
    nes.runFrame(true);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("%d frames in %.3f s, %.0f fps\n", frames, s, frames / s);
  return 0;
}


int main(int argc, char ** argv)
{
  if (argc >= 4 && !strcmp(argv[1], "nestest"))
    return nestest(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "run"))
    return run(argc, argv);
  if (argc >= 4 && !strcmp(argv[1], "bench"))
    return bench(argv[2], atoi(argv[3]));
  fprintf(stderr, "usage: see the comment at the top of host/main.cpp\n");
  return 1;
}
