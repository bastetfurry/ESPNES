#include <stdlib.h>
#include <string.h>

#include "nsf.h"
#include "nes.h"


bool isNsf(uint8_t const * image, size_t size)
{
  return size >= 0x80 && !memcmp(image, "NESM\x1a", 5);
}


static uint16_t word(uint8_t const * p)
{
  return p[0] | p[1] << 8;
}


char const * parseNsf(uint8_t const * image, size_t size, NsfInfo * info)
{
  if (!isNsf(image, size))
    return "Not an NSF file";
  memset(info, 0, sizeof(*info));
  info->songs = image[6] ? image[6] : 1;
  info->firstSong = image[7] >= 1 && image[7] <= info->songs ? image[7] : 1;
  info->load = word(image + 8);
  info->init = word(image + 10);
  info->play = word(image + 12);
  for (int i = 0; i < 3; ++i) {
    char * out = i == 0 ? info->title : i == 1 ? info->artist : info->copyright;
    memcpy(out, image + 0x0e + 32 * i, 32);
    out[32] = 0;
  }
  info->periodNtsc = word(image + 0x6e) ? word(image + 0x6e) : 16639;
  info->periodPal = word(image + 0x78) ? word(image + 0x78) : 19997;
  memcpy(info->banks, image + 0x70, 8);
  for (int i = 0; i < 8; ++i)
    info->bankSwitched |= info->banks[i] != 0;
  info->pal = image[0x7a] & 1;
  info->dual = image[0x7a] & 2;
  info->chips = image[0x7b];
  info->data = image + 0x80;
  info->dataSize = size - 0x80;
  if (info->chips & ~(NSF_FDS | NSF_VRC6 | NSF_MMC5 | NSF_5B | NSF_N163))
    return "This NSF needs a sound chip ESPNES does not have yet";
  if (!(info->chips & NSF_FDS) && !info->bankSwitched && info->load < 0x8000)
    return "NSF load address below $8000";
  return nullptr;
}


class MapperNSF : public Mapper {
public:
  MapperNSF(Nes & nes, NsfInfo const & info) : Mapper(nes), m_info(info)
  {
    m_ram = (uint8_t *) malloc(0x8000);
  }
  ~MapperNSF()                                 { free(m_ram); }
  bool ok() const                              { return m_ram != nullptr; }

  void reset() override
  {
    memset(m_ram, 0, 0x8000);
    restore();
    load();
  }

  void restore() override
  {
    for (int i = 0; i < 4; ++i)
      m_nes.mapPrg8K(i, m_ram + i * 0x2000);
    // the PPU has nothing to show, but some music reads or writes it: CHR RAM, nametables
    m_nes.mapChr8K(0);
    m_nes.setMirroring(Mirroring::Vertical);
  }

  // the music's data, as at power on: the initial banks, or the data at its load address
  void load()
  {
    bool fds = m_info.chips & NSF_FDS;
    if (m_info.bankSwitched) {
      for (int i = 0; i < 8; ++i)
        bank(8 + i, m_info.banks[i]);
      if (fds) {
        bank(6, m_info.banks[6]);
        bank(7, m_info.banks[7]);
      }
      return;
    }
    for (size_t i = 0; i < m_info.dataSize; ++i) {
      uint32_t addr = m_info.load + i;
      if (addr >= 0x10000)
        break;
      if (addr >= 0x8000)
        m_ram[addr - 0x8000] = m_info.data[i];
      else if (fds && addr >= 0x6000)
        m_nes.prgRam[addr - 0x6000] = m_info.data[i];
    }
  }

  // 4 KB bank b of the data into slot 'slot' (6-15: $6000-$FFFF)
  void bank(int slot, uint8_t b)
  {
    uint8_t * to = slot >= 8 ? m_ram + (slot - 8) * 0x1000 : m_nes.prgRam + (slot - 6) * 0x1000;
    int32_t from = b * 0x1000 - (m_info.load & 0xfff);
    for (int i = 0; i < 0x1000; ++i) {
      int32_t at = from + i;
      to[i] = at >= 0 && at < (int32_t) m_info.dataSize ? m_info.data[at] : 0;
    }
  }

  void writeLow(uint16_t addr, uint8_t value) override
  {
    if (m_info.chips & NSF_MMC5) {
      // the MMC5's multiplier and its 1 KB ExRAM, which MMC5 music may use
      if (addr == 0x5205 || addr == 0x5206) {
        m_factor[addr - 0x5205] = value;
        return;
      }
      if (addr >= 0x5c00 && addr < 0x5ff6) {
        m_exram[addr - 0x5c00] = value;
        return;
      }
    }
    if (addr >= 0x5ff8)
      bank(8 + (addr - 0x5ff8), value);
    else if ((addr == 0x5ff6 || addr == 0x5ff7) && (m_info.chips & NSF_FDS))
      bank(6 + (addr - 0x5ff6), value);
  }

  int readLow(uint16_t addr) override
  {
    // the idle loop: JMP $4100
    static const uint8_t IDLE[3] = { 0x4c, NSF_IDLE & 0xff, NSF_IDLE >> 8 };
    if (addr >= NSF_IDLE && addr < NSF_IDLE + 3)
      return IDLE[addr - NSF_IDLE];
    if (m_info.chips & NSF_MMC5) {
      unsigned product = m_factor[0] * m_factor[1];
      if (addr == 0x5205)
        return product & 0xff;
      if (addr == 0x5206)
        return product >> 8;
      if (addr >= 0x5c00 && addr < 0x5ff6)
        return m_exram[addr - 0x5c00];
    }
    return -1;
  }

  // FDS music writes to its RAM at $8000-$FFFF
  void writeRegister(uint16_t addr, uint8_t value) override
  {
    if (m_info.chips & NSF_FDS)
      m_ram[addr - 0x8000] = value;
  }

private:
  NsfInfo const & m_info;
  uint8_t *       m_ram;
  uint8_t         m_factor[2] = { 0xff, 0xff };
  uint8_t         m_exram[0x3f6] = { };
};


Mapper * createNsfMapper(Nes & nes, NsfInfo const & info)
{
  MapperNSF * m = new MapperNSF(nes, info);
  if (!m->ok()) {
    delete m;
    return nullptr;
  }
  return m;
}
