#include "serial_port.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mowbot_dock
{
namespace
{

// A frame is ~40 bytes; anything beyond this is a garbage flood.
constexpr size_t kMaxRxBuffer = 4096;

speed_t to_speed(int baud)
{
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B0;
  }
}

}  // namespace

SerialPort::~SerialPort()
{
  close();
}

bool SerialPort::open(const std::string & device, int baud)
{
  close();

  const speed_t speed = to_speed(baud);
  if (speed == B0) {
    last_error_ = "unsupported baud rate " + std::to_string(baud);
    return false;
  }

  fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    last_error_ = device + ": " + std::strerror(errno);
    return false;
  }

  termios tio{};
  if (tcgetattr(fd_, &tio) != 0) {
    last_error_ = std::string("tcgetattr: ") + std::strerror(errno);
    close();
    return false;
  }
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  // VMIN=1 + O_NONBLOCK: an empty port reads as EAGAIN. (VMIN=0 would make
  // read() return 0 for "no data", indistinguishable from EOF.)
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);
  if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
    last_error_ = std::string("tcsetattr: ") + std::strerror(errno);
    close();
    return false;
  }

  tcflush(fd_, TCIOFLUSH);
  rx_buffer_.clear();
  return true;
}

void SerialPort::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  rx_buffer_.clear();
}

bool SerialPort::read_lines(std::vector<std::string> & lines)
{
  if (fd_ < 0) {
    last_error_ = "port not open";
    return false;
  }

  char buf[512];
  while (true) {
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n > 0) {
      rx_buffer_.append(buf, static_cast<size_t>(n));
      if (rx_buffer_.size() > kMaxRxBuffer) {
        rx_buffer_.clear();  // garbage flood; resync on the next newline
      }
      continue;
    }
    if (n == 0) {
      // Treat as "no data": disconnects (USB unplug, pty peer close)
      // surface as EIO below, and the node's stale-data warning covers a
      // silently dead port.
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    last_error_ = std::string("read: ") + std::strerror(errno);
    return false;
  }

  size_t start = 0;
  while (true) {
    const size_t nl = rx_buffer_.find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    size_t end = nl;
    while (end > start && rx_buffer_[end - 1] == '\r') {
      --end;
    }
    if (end > start) {
      lines.emplace_back(rx_buffer_, start, end - start);
    }
    start = nl + 1;
  }
  rx_buffer_.erase(0, start);
  return true;
}

bool SerialPort::write_line(const std::string & line)
{
  if (fd_ < 0) {
    last_error_ = "port not open";
    return false;
  }

  const std::string out = line + "\n";
  size_t written = 0;
  // At 115200 baud a command frame drains in ~1 ms; bound the retries so a
  // wedged port can't stall the node.
  for (int attempts = 0; written < out.size() && attempts < 50; ++attempts) {
    const ssize_t n = ::write(fd_, out.data() + written, out.size() - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ::usleep(1000);
      continue;
    }
    last_error_ = std::string("write: ") + std::strerror(errno);
    return false;
  }
  if (written < out.size()) {
    last_error_ = "write: output buffer stalled";
    return false;
  }
  return true;
}

}  // namespace mowbot_dock
