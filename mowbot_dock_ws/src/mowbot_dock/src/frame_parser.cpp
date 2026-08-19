#include "frame_parser.hpp"

#include <charconv>
#include <string_view>
#include <vector>

namespace mowbot_dock
{
namespace
{

std::string_view trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string_view> split_fields(std::string_view line)
{
  std::vector<std::string_view> fields;
  size_t start = 0;
  while (true) {
    const size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      fields.push_back(trim(line.substr(start)));
      return fields;
    }
    fields.push_back(trim(line.substr(start, comma - start)));
    start = comma + 1;
  }
}

// from_chars: locale-independent, requires the whole field to be consumed.
template<typename T>
bool parse_number(std::string_view s, T & out)
{
  if (s.empty()) {
    return false;
  }
  const auto result = std::from_chars(s.data(), s.data() + s.size(), out);
  return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

bool parse_bool(std::string_view s, bool & out)
{
  int value = 0;
  if (!parse_number(s, value) || (value != 0 && value != 1)) {
    return false;
  }
  out = value == 1;
  return true;
}

}  // namespace

std::optional<StatusFrame> parse_status_frame(const std::string & line)
{
  const auto fields = split_fields(line);
  if (fields.size() != 8) {
    return std::nullopt;
  }

  StatusFrame frame{};
  int64_t uptime = 0;
  if (!parse_number(fields[0], frame.state) ||
    !parse_bool(fields[1], frame.microswitch) ||
    !parse_bool(fields[2], frame.k1) ||
    !parse_bool(fields[3], frame.k2) ||
    !parse_number(fields[4], frame.charge_current) ||
    !parse_number(fields[5], frame.charger_voltage) ||
    !parse_number(fields[6], frame.fault_code) ||
    !parse_number(fields[7], uptime))
  {
    return std::nullopt;
  }
  if (frame.state < 0 || frame.state > 7 || frame.fault_code < 0 ||
    uptime < 0 || uptime > UINT32_MAX)
  {
    return std::nullopt;
  }
  frame.uptime_s = static_cast<uint32_t>(uptime);
  return frame;
}

bool is_event_line(const std::string & line)
{
  return line.rfind("EVT:", 0) == 0;
}

const char * state_name(int state)
{
  static const char * const kNames[] = {
    "IDLE", "SEATED", "RAMP", "CHARGING", "DRAIN", "COMPLETE", "SELFTEST", "FAULT"};
  return (state >= 0 && state <= 7) ? kNames[state] : "?";
}

const char * fault_name(int fault_code)
{
  static const char * const kNames[] = {
    "none", "overcurrent", "AC relay weld", "no 42V after AC-on", "watchdog silence"};
  return (fault_code >= 0 && fault_code <= 4) ? kNames[fault_code] : "?";
}

}  // namespace mowbot_dock
