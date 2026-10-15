#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "compat.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_vfs_dev.h"


static constexpr char const * MOUNT = "/spiffs";


void delay(uint32_t ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms));
}


void delayMicroseconds(uint32_t us)
{
  esp_rom_delay_us(us);
}


void pinMode(int pin, int mode)
{
  gpio_config_t c = { };
  c.pin_bit_mask = 1ULL << pin;
  c.mode         = mode == OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
  c.pull_up_en   = mode == INPUT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  gpio_config(&c);
}


///////////////////////////////////////////////////////////////////////////////
// String

void String::trim()
{
  size_t a = 0, b = size();
  while (a < b && isspace((unsigned char) (*this)[a]))
    ++a;
  while (b > a && isspace((unsigned char) (*this)[b - 1]))
    --b;
  *this = String(substr(a, b - a));
}


void String::toLowerCase()
{
  for (auto & c : *this)
    c = tolower((unsigned char) c);
}


///////////////////////////////////////////////////////////////////////////////
// Serial

SerialPort Serial;


// The UART driver with buffers, and stdout through it, so printing does not wait for the wire
// (the statistics line would otherwise stall the emulation for 13 ms)
void SerialPort::begin(int baud)
{
  uart_driver_install(UART_NUM_0, 512, m_txBuffer, 0, nullptr, 0);
  uart_set_baudrate(UART_NUM_0, baud);
  // hand over each byte at once, not after a pause in the input (a command and its number
  // arrive together)
  uart_set_rx_full_threshold(UART_NUM_0, 1);
  uart_set_rx_timeout(UART_NUM_0, 1);
  esp_vfs_dev_uart_use_driver(UART_NUM_0);
  esp_vfs_dev_uart_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
  setvbuf(stdout, nullptr, _IONBF, 0);
}


int SerialPort::available()
{
  if (m_peeked >= 0)
    return 1;
  size_t n = 0;
  uart_get_buffered_data_len(UART_NUM_0, &n);
  return n;
}


int SerialPort::read()
{
  if (m_peeked >= 0) {
    int c = m_peeked;
    m_peeked = -1;
    return c;
  }
  uint8_t c;
  return uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1 ? c : -1;
}


int SerialPort::peek()
{
  if (m_peeked < 0)
    m_peeked = read();
  return m_peeked;
}


int SerialPort::timedRead()
{
  if (m_peeked >= 0)
    return read();
  uint8_t c;
  return uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(1000)) == 1 ? c : -1;
}


// as Arduino's Stream::parseInt: skips what is not a digit or '-', then reads digits
long SerialPort::parseInt()
{
  int c;
  do {
    c = timedRead();
    if (c < 0)
      return 0;
  } while (c != '-' && !isdigit(c));
  bool negative = c == '-';
  long v = negative ? 0 : c - '0';
  for (;;) {
    c = peek();
    if (c < 0) {
      // wait a little for more digits, as the timeout would
      c = timedRead();
      if (c < 0)
        break;
      m_peeked = c;
    }
    if (!isdigit(c))
      break;
    read();
    v = v * 10 + (c - '0');
  }
  return negative ? -v : v;
}


void SerialPort::printf(char const * format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}


void SerialPort::print(char const * s)
{
  fputs(s, stdout);
}


void SerialPort::println(char const * s)
{
  fputs(s, stdout);
  fputc('\n', stdout);
}


///////////////////////////////////////////////////////////////////////////////
// SPIFFS

SpiffsFS SPIFFS;


static String full(String const & path)
{
  return String(MOUNT) + path;
}


File & File::operator=(File && o)
{
  close();
  m_f = o.m_f;
  m_name = o.m_name;
  m_dir = o.m_dir;
  m_dirHandle = o.m_dirHandle;
  o.m_f = nullptr;
  o.m_dirHandle = nullptr;
  return *this;
}


void File::close()
{
  if (m_f)
    fclose(m_f);
  if (m_dirHandle)
    closedir((DIR *) m_dirHandle);
  m_f = nullptr;
  m_dirHandle = nullptr;
}


size_t File::size()
{
  if (!m_f)
    return 0;
  long pos = ftell(m_f);
  fseek(m_f, 0, SEEK_END);
  long end = ftell(m_f);
  fseek(m_f, pos, SEEK_SET);
  return end;
}


int File::available()
{
  if (!m_f)
    return 0;
  long pos = ftell(m_f);
  return (int)(size() - pos);
}


String File::readStringUntil(char end)
{
  String s;
  int c;
  while (m_f && (c = fgetc(m_f)) != EOF && c != end)
    s += (char) c;
  return s;
}


File File::openNextFile()
{
  if (!m_dirHandle)
    return File();
  for (;;) {
    struct dirent * e = readdir((DIR *) m_dirHandle);
    if (!e)
      return File();
    String name = String("/") + e->d_name;
    FILE * f = fopen(full(name).c_str(), "r");
    if (f)
      return File(f, e->d_name);       // the name without a slash, as Arduino's SPIFFS gives it
  }
}


bool SpiffsFS::begin(bool formatOnFail)
{
  esp_vfs_spiffs_conf_t conf = { };
  conf.base_path = MOUNT;
  conf.partition_label = nullptr;
  conf.max_files = 6;
  conf.format_if_mount_failed = formatOnFail;
  return esp_vfs_spiffs_register(&conf) == ESP_OK;
}


File SpiffsFS::open(String const & path, char const * mode)
{
  if (path == "/") {
    File d;
    d.m_dirHandle = opendir(MOUNT);
    d.m_dir = true;
    return d;
  }
  // Arduino's "w" truncates, "r" reads; binary is the same here
  FILE * f = fopen(full(path).c_str(), mode);
  return f ? File(f, path) : File();
}


bool SpiffsFS::exists(String const & path)
{
  struct stat st;
  return stat(full(path).c_str(), &st) == 0;
}


bool SpiffsFS::remove(String const & path)
{
  return ::remove(full(path).c_str()) == 0;
}


bool SpiffsFS::rename(String const & from, String const & to)
{
  return ::rename(full(from).c_str(), full(to).c_str()) == 0;
}


size_t SpiffsFS::totalBytes()
{
  size_t total = 0, used = 0;
  esp_spiffs_info(nullptr, &total, &used);
  return total;
}


size_t SpiffsFS::usedBytes()
{
  size_t total = 0, used = 0;
  esp_spiffs_info(nullptr, &total, &used);
  return used;
}
