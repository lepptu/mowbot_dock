// Parsing for the dock Nano serial protocol (mowbot_plans/Docking plan/
// 02_ARDUINO_FIRMWARE.md): ASCII CSV status frames and EVT: event lines.
// No ROS or I/O dependencies so it can be unit-tested standalone.
#ifndef MOWBOT_DOCK__FRAME_PARSER_HPP_
#define MOWBOT_DOCK__FRAME_PARSER_HPP_

#include <cstdint>
#include <optional>
#include <string>

namespace mowbot_dock
{

// One status frame:
// <state>,<microswitch>,<k1>,<k2>,<chargeCurrent>,<chargerVoltage>,<faultCode>,<uptimeS>
struct StatusFrame
{
  int state;               // 0..7: IDLE SEATED RAMP CHARGING DRAIN COMPLETE SELFTEST FAULT
  bool microswitch;        // robot seated
  bool k1;                 // 42 V DC relay commanded state
  bool k2;                 // mains pilot relay commanded state
  float charge_current;    // A, filtered
  float charger_voltage;   // V, divider #1 (~0 when AC off)
  int fault_code;          // 0 none, 1 overcurrent, 2 AC weld, 3 no 42V, 4 watchdog
  uint32_t uptime_s;       // resets to 0 on DTR reboot
};

// Returns the parsed frame, or nullopt for anything malformed: wrong field
// count, non-numeric fields, booleans outside 0/1, state outside 0..7 or a
// negative fault code. Whitespace around fields is tolerated; fault codes
// above 4 are passed through for forward compatibility.
std::optional<StatusFrame> parse_status_frame(const std::string & line);

// True for event lines ("EVT:<TYPE>[:detail]").
bool is_event_line(const std::string & line);

const char * state_name(int state);        // "IDLE".."FAULT" or "?"
const char * fault_name(int fault_code);   // "none".."watchdog silence" or "?"

}  // namespace mowbot_dock

#endif  // MOWBOT_DOCK__FRAME_PARSER_HPP_
