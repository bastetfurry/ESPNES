// PC test harness for the emulator core (src/nes). Not part of the ESP32 build.
//
//   nestest <nestest.nes> <nestest.log>   run nestest's automated mode, compare with the golden log
//   run <rom.nes> <frames> <out.ppm> [pal] [hold=BUTTONS@FROM-TO ...]
//                                         run frames and write the last one as PPM; BUTTONS is
//                                         a string of A B s(elect) S(tart) U D L R
//   bench <rom.nes> <frames>              time frames with drawing, for a rough relative speed

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <chrono>

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


static uint8_t s_frame[240][256];

static void sink(void *, int y, uint8_t const * colors)
{
  memcpy(s_frame[y], colors, 256);
}


struct Hold { uint8_t buttons; int from, to; };

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
  for (int i = 5; i < argc; ++i) {
    if (!strcmp(argv[i], "pal"))
      nes.setRegion(Region::PAL);
    else if (!strncmp(argv[i], "hold=", 5)) {
      Hold h;
      h.buttons = parseButtons(argv[i] + 5);
      sscanf(strchr(argv[i], '@'), "@%d-%d", &h.from, &h.to);
      holds.push_back(h);
    }
  }
  nes.setLineSink(sink, nullptr);
  nes.powerOn();
  for (int f = 0; f < frames; ++f) {
    uint8_t b = 0;
    for (auto & h : holds)
      if (f >= h.from && f <= h.to)
        b |= h.buttons;
    nes.setButtons(0, b);
    nes.runFrame(true);
    if (nes.cpu.jammed) {
      printf("CPU jammed at $%04X in frame %d\n", nes.cpu.PC, f);
      break;
    }
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
  nes.setLineSink(sink, nullptr);
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
