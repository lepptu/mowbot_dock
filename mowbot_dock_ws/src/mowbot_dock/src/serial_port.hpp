// RAII non-blocking termios serial port.
//
// Opening the port asserts DTR, which resets the Nano by design: both relays
// drop via hardware pulldowns and the firmware reboots (sends EVT:BOOT).
// This is the documented recovery path, not a fault.
#ifndef MOWBOT_DOCK__SERIAL_PORT_HPP_
#define MOWBOT_DOCK__SERIAL_PORT_HPP_

#include <string>
#include <vector>

namespace mowbot_dock
{

class SerialPort
{
public:
  SerialPort() = default;
  ~SerialPort();
  SerialPort(const SerialPort &) = delete;
  SerialPort & operator=(const SerialPort &) = delete;

  // Opens and configures the device (raw 8N1, non-blocking). Flushes any
  // garbage pending from the DTR reset. False on failure (see last_error()).
  bool open(const std::string & device, int baud);
  void close();
  bool is_open() const {return fd_ >= 0;}

  // Drains available bytes and appends complete lines (terminator and any
  // trailing \r stripped, empty lines skipped) to `lines`. Returns false on
  // an unrecoverable I/O error or EOF — caller should close and reconnect.
  bool read_lines(std::vector<std::string> & lines);

  // Writes `line` plus '\n'. False on error.
  bool write_line(const std::string & line);

  const std::string & last_error() const {return last_error_;}

private:
  int fd_{-1};
  std::string rx_buffer_;
  std::string last_error_;
};

}  // namespace mowbot_dock

#endif  // MOWBOT_DOCK__SERIAL_PORT_HPP_
