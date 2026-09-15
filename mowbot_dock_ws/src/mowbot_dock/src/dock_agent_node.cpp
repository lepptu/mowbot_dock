// dock_agent_node: bridges the dock Arduino Nano (USB serial) to ROS 2.
//
// RX: 10 Hz status frames -> dock/* topics + dock/battery_state.
// TX: 10 Hz command frames <chargeEnable>,<clearFault>,<ledMode>,<selfTest>
//     (also the Nano's watchdog feed; the Nano tolerates 2000 ms silence).
//
// clearFault and selfTest are edge-triggered on the Nano and must return to
// 0 after the pulse, so a `true` on the corresponding command topic is
// stretched over a few frames and then auto-cleared.
//
// Runtime parameters (firmware >= 0.2.4): the charge-complete criterion and
// the top-up interval are ROS parameters here and mirrored to the Nano with
// `SET:<NAME>:<value>` lines. The Nano keeps them in RAM only, so the full
// set is re-sent after every `EVT:BOOT` (each port open DTR-resets it). The
// values the Nano actually holds come back as `EVT:CFG:<NAME>:<value>` and
// are published latched on dock/config/<param>.
//
// Nothing is transmitted between opening the port and EVT:BOOT (or, as a
// fallback, 3 s of status frames): the Nano's 57600-baud bootloader reads
// our 115200-baud bytes as garbage and can sit waiting for "data" for
// minutes, leaving the dock silent (seen on the real dock 2026-09-15).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rcl_interfaces/msg/floating_point_range.hpp"
#include "rcl_interfaces/msg/integer_range.hpp"
#include "rcl_interfaces/msg/parameter_descriptor.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/battery_state.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/float32.hpp"
#include "std_msgs/msg/int32.hpp"
#include "std_msgs/msg/string.hpp"

#include "frame_parser.hpp"
#include "serial_port.hpp"

using namespace std::chrono_literals;
using sensor_msgs::msg::BatteryState;

namespace mowbot_dock
{

// One Nano runtime parameter. Clamps/precision mirror the firmware (0.2.4):
// the agent rejects anything outside them so the ROS parameter and the value
// the Nano applies never disagree.
struct NanoParam
{
  const char * ros_name;
  const char * nano_name;
  bool integer;
  double min;
  double max;
  int decimals;         // doubles: digits the firmware keeps
  bool zero_means_off;  // 0 is valid even though it is below `min`
  double default_value;
  const char * description;
};

constexpr NanoParam kNanoParams[] = {
  {"complete_a", "COMPLETE_A", false, 0.05, 3.0, 2, false, 0.65,
    "Charge-complete current threshold [A]: taper ends once the charger "
    "current stays below this (0.05-3.0, 2 decimals)"},
  {"complete_v_min", "COMPLETE_V_MIN", false, 0.0, 60.0, 1, false, 41.0,
    "Charge-complete minimum charger-side voltage [V] (0-60, 1 decimal)"},
  {"complete_s", "COMPLETE_S", true, 1, 3600, 0, false, 300,
    "Charge-complete taper window [s]: current must stay below complete_a "
    "with voltage >= complete_v_min for this long (1-3600)"},
  {"topup_interval_s", "TOPUP_INTERVAL_S", true, 60, 604800, 0, true, 0,
    "Firmware top-up: re-sequence a charge this long after COMPLETE [s] "
    "(0 = off, else 60-604800)"},
};

const NanoParam * find_nano_param(const std::string & ros_name)
{
  for (const auto & p : kNanoParams) {
    if (ros_name == p.ros_name) {
      return &p;
    }
  }
  return nullptr;
}

// int64 or double parameter as a double (the types are fixed at declaration).
double numeric_value(const rclcpp::Parameter & param)
{
  return param.get_type() == rclcpp::ParameterType::PARAMETER_INTEGER ?
         static_cast<double>(param.as_int()) : param.as_double();
}

// Value as the firmware wants it on the wire: fixed decimals, no exponent.
std::string format_nano_value(const NanoParam & p, double value)
{
  char buf[32];
  if (p.integer) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(value)));
  } else {
    std::snprintf(buf, sizeof(buf), "%.*f", p.decimals, value);
  }
  return buf;
}

class DockAgentNode : public rclcpp::Node
{
public:
  DockAgentNode()
  : Node("dock_agent")
  {
    device_ = declare_parameter<std::string>("serial_device", "/dev/ttyUSB0");
    baud_ = static_cast<int>(declare_parameter<int64_t>("baud", 115200));
    const double command_rate = declare_parameter<double>("command_rate_hz", 10.0);
    reconnect_interval_ = rclcpp::Duration::from_seconds(
      declare_parameter<double>("reconnect_interval_s", 2.0));
    pulse_frames_ = static_cast<int>(declare_parameter<int64_t>("pulse_frames", 3));
    charge_enable_ = declare_parameter<bool>("charge_enable_default", true);
    v_empty_ = declare_parameter<double>("battery_voltage_empty", 33.0);
    v_full_ = declare_parameter<double>("battery_voltage_full", 42.0);
    // Validation is registered before the declarations so an out-of-range
    // value in the params file (e.g. topup_interval_s 30) is refused at
    // declaration and replaced by the default instead of being held here
    // while the Nano clamps it to something else.
    on_set_cb_ = add_on_set_parameters_callback(
      [this](const std::vector<rclcpp::Parameter> & params) {
        return validate_nano_params(params);
      });
    declare_nano_params();

    // Latched topics mirror the MQTT-retained set from the plan so late
    // joiners (docking server, bridge) see the last value immediately.
    const auto latched = rclcpp::QoS(1).transient_local();
    const auto stream = rclcpp::QoS(10);

    pub_state_ = create_publisher<std_msgs::msg::Int32>("dock/state", latched);
    pub_microswitch_ = create_publisher<std_msgs::msg::Bool>("dock/microswitch", latched);
    pub_relay_k1_ = create_publisher<std_msgs::msg::Bool>("dock/relay_k1", latched);
    pub_relay_k2_ = create_publisher<std_msgs::msg::Bool>("dock/relay_k2", latched);
    pub_fault_ = create_publisher<std_msgs::msg::Int32>("dock/fault", latched);
    pub_self_test_result_ =
      create_publisher<std_msgs::msg::Bool>("dock/self_test_result", latched);
    pub_charge_current_ =
      create_publisher<std_msgs::msg::Float32>("dock/charge_current", stream);
    pub_charger_voltage_ =
      create_publisher<std_msgs::msg::Float32>("dock/charger_voltage", stream);
    pub_event_ = create_publisher<std_msgs::msg::String>("dock/event", stream);
    pub_battery_state_ = create_publisher<BatteryState>("dock/battery_state", stream);
    // Echo of the charge permission + firmware version for the MQTT bridge /
    // web UI (05_WEB_UI.md §2.1): latched, published on change only.
    pub_charge_enable_ = create_publisher<std_msgs::msg::Bool>("dock/charge_enable", latched);
    pub_firmware_version_ =
      create_publisher<std_msgs::msg::String>("dock/firmware_version", latched);
    // Applied (clamped) runtime parameters as echoed by the Nano; latched so
    // the web UI can show what the dock really runs next to the ROS value.
    for (const auto & p : kNanoParams) {
      const std::string topic = std::string("dock/config/") + p.ros_name;
      if (p.integer) {
        pub_config_int_[p.nano_name] = create_publisher<std_msgs::msg::Int32>(topic, latched);
      } else {
        pub_config_float_[p.nano_name] =
          create_publisher<std_msgs::msg::Float32>(topic, latched);
      }
    }

    sub_charge_enable_ = create_subscription<std_msgs::msg::Bool>(
      "dock/charge_enable_cmd", stream,
      [this](const std_msgs::msg::Bool & msg) {
        if (charge_enable_ != msg.data) {
          RCLCPP_INFO(get_logger(), "charge_enable -> %s", msg.data ? "true" : "false");
          charge_enable_ = msg.data;
          publish_bool(*pub_charge_enable_, charge_enable_);
        }
      });
    sub_clear_fault_ = create_subscription<std_msgs::msg::Bool>(
      "dock/clear_fault_cmd", stream,
      [this](const std_msgs::msg::Bool & msg) {
        if (msg.data) {
          clear_fault_pulse_ = pulse_frames_;
          RCLCPP_INFO(get_logger(), "clearFault pulse requested");
        }
      });
    sub_self_test_ = create_subscription<std_msgs::msg::Bool>(
      "dock/self_test_cmd", stream,
      [this](const std_msgs::msg::Bool & msg) {
        if (msg.data) {
          self_test_pulse_ = pulse_frames_;
          RCLCPP_INFO(get_logger(), "selfTest pulse requested");
        }
      });

    const auto tx_period =
      std::chrono::duration<double>(1.0 / std::max(command_rate, 1.0));
    tx_timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::milliseconds>(tx_period),
      [this]() {tx_tick();});
    rx_timer_ = create_wall_timer(20ms, [this]() {rx_tick();});

    last_rx_time_ = now();
    publish_bool(*pub_charge_enable_, charge_enable_);  // initial (default) permission

    // Registered after the declarations: the initial values are carried by
    // the EVT:BOOT sync, this only forwards later changes.
    post_set_cb_ = add_post_set_parameters_callback(
      [this](const std::vector<rclcpp::Parameter> & params) {
        for (const auto & param : params) {
          if (const NanoParam * p = find_nano_param(param.get_name())) {
            send_nano_param(*p, numeric_value(param));
          }
        }
      });

    try_open();
  }

private:
  // ── Nano runtime parameters ──────────────────────────────────────────────

  void declare_nano_params()
  {
    for (const auto & p : kNanoParams) {
      rcl_interfaces::msg::ParameterDescriptor desc;
      desc.name = p.ros_name;
      desc.description = p.description;
      if (p.integer) {
        rcl_interfaces::msg::IntegerRange r;
        r.from_value = p.zero_means_off ? 0 : static_cast<int64_t>(p.min);
        r.to_value = static_cast<int64_t>(p.max);
        r.step = 0;
        desc.integer_range.push_back(r);
      } else {
        rcl_interfaces::msg::FloatingPointRange r;
        r.from_value = p.min;
        r.to_value = p.max;
        r.step = 0.0;
        desc.floating_point_range.push_back(r);
      }
      try {
        if (p.integer) {
          declare_parameter<int64_t>(p.ros_name, static_cast<int64_t>(p.default_value), desc);
        } else {
          declare_parameter<double>(p.ros_name, p.default_value, desc);
        }
      } catch (const rclcpp::exceptions::InvalidParameterValueException & e) {
        // A bad value in the params file must not take the whole agent down:
        // fall back to the firmware default and say so.
        RCLCPP_ERROR(
          get_logger(), "parameter %s: %s -> using default %s", p.ros_name, e.what(),
          format_nano_value(p, p.default_value).c_str());
        if (p.integer) {
          declare_parameter<int64_t>(
            p.ros_name, static_cast<int64_t>(p.default_value), desc, true);
        } else {
          declare_parameter<double>(p.ros_name, p.default_value, desc, true);
        }
      }
    }
  }

  rcl_interfaces::msg::SetParametersResult validate_nano_params(
    const std::vector<rclcpp::Parameter> & params)
  {
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;
    for (const auto & param : params) {
      const NanoParam * p = find_nano_param(param.get_name());
      if (p == nullptr) {
        continue;
      }
      const double v = numeric_value(param);
      std::string why;
      if (p->zero_means_off && v == 0.0) {
        // explicitly allowed
      } else if (v < p->min || v > p->max) {
        why = "must be " + std::string(p->zero_means_off ? "0 (off) or " : "") + "in [" +
          format_nano_value(*p, p->min) + ", " + format_nano_value(*p, p->max) + "]";
      } else if (!p->integer) {
        const double scaled = v * std::pow(10.0, p->decimals);
        if (std::fabs(scaled - std::round(scaled)) > 1e-6) {
          why = "must have at most " + std::to_string(p->decimals) +
            " decimals (the firmware keeps " + format_nano_value(*p, v) + ")";
        }
      }
      if (!why.empty()) {
        result.successful = false;
        result.reason = std::string(p->ros_name) + " " + why;
        RCLCPP_WARN(get_logger(), "rejected: %s", result.reason.c_str());
        return result;
      }
    }
    return result;
  }

  // Writes one SET line if the Nano is past its boot; otherwise the pending
  // EVT:BOOT sync will carry the value.
  void send_nano_param(const NanoParam & p, double value)
  {
    if (!port_.is_open() || !nano_ready_) {
      RCLCPP_INFO(
        get_logger(), "%s = %s (Nano not ready, applied at next boot sync)",
        p.ros_name, format_nano_value(p, value).c_str());
      return;
    }
    const std::string line =
      std::string("SET:") + p.nano_name + ":" + format_nano_value(p, value);
    RCLCPP_INFO(get_logger(), "-> %s", line.c_str());
    if (!port_.write_line(line)) {
      drop_connection(port_.last_error());
    }
  }

  // Marks the firmware as running (TX may start) and sends all four
  // parameters: the Nano holds them in RAM only and comes up with the
  // firmware defaults after every reset. Must run after EVT:BOOT — bytes
  // written during the bootloader window are lost or, worse, keep the
  // bootloader busy.
  void sync_nano_config(const char * why)
  {
    RCLCPP_INFO(get_logger(), "Nano running: syncing runtime parameters (%s)", why);
    nano_ready_ = true;
    frames_since_sync_ = 0;
    for (const auto & p : kNanoParams) {
      send_nano_param(p, numeric_value(get_parameter(p.ros_name)));
      if (!port_.is_open()) {
        return;
      }
    }
  }

  void handle_config_event(const ConfigEvent & evt)
  {
    if (const auto it = pub_config_float_.find(evt.name); it != pub_config_float_.end()) {
      publish_float(*it->second, static_cast<float>(evt.value));
    } else if (const auto jt = pub_config_int_.find(evt.name); jt != pub_config_int_.end()) {
      publish_int(*jt->second, static_cast<int>(std::llround(evt.value)));
    } else {
      RCLCPP_INFO(get_logger(), "unknown Nano parameter echoed: %s", evt.name.c_str());
    }
  }

  // ── Serial ───────────────────────────────────────────────────────────────

  void try_open()
  {
    last_connect_attempt_ = now();
    if (port_.open(device_, baud_)) {
      RCLCPP_INFO(
        get_logger(), "opened %s @ %d baud (DTR reset: Nano rebooting)",
        device_.c_str(), baud_);
      last_rx_time_ = now();
      port_opened_ = last_rx_time_;
      nano_ready_ = false;  // bootloader window: hold all TX until EVT:BOOT
    } else {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 10000, "cannot open %s: %s",
        device_.c_str(), port_.last_error().c_str());
    }
  }

  void drop_connection(const std::string & why)
  {
    RCLCPP_ERROR(get_logger(), "serial connection lost (%s), reconnecting", why.c_str());
    port_.close();
    nano_ready_ = false;
  }

  void tx_tick()
  {
    if (!port_.is_open() || !nano_ready_) {
      return;
    }
    const char clear_fault = clear_fault_pulse_ > 0 ? '1' : '0';
    const char self_test = self_test_pulse_ > 0 ? '1' : '0';
    if (clear_fault_pulse_ > 0) {--clear_fault_pulse_;}
    if (self_test_pulse_ > 0) {--self_test_pulse_;}

    std::string frame;
    frame += charge_enable_ ? '1' : '0';
    frame += ',';
    frame += clear_fault;
    frame += ",0,";  // ledMode reserved (0 = auto)
    frame += self_test;
    if (!port_.write_line(frame)) {
      drop_connection(port_.last_error());
    }
  }

  void rx_tick()
  {
    if (!port_.is_open()) {
      if (now() - last_connect_attempt_ >= reconnect_interval_) {
        try_open();
      }
      return;
    }

    std::vector<std::string> lines;
    if (!port_.read_lines(lines)) {
      drop_connection(port_.last_error());
      return;
    }
    for (const auto & line : lines) {
      if (is_event_line(line)) {
        handle_event(line);
      } else if (auto frame = parse_status_frame(line)) {
        handle_status(*frame);
      } else if (is_warning_line(line)) {
        // e.g. "WARNING:SET: unknown parameter" — nothing was applied.
        RCLCPP_WARN(get_logger(), "Nano: %s", line.c_str());
        std_msgs::msg::String msg;
        msg.data = line;
        pub_event_->publish(msg);
      } else if (const auto evt = line.find("EVT:"); evt != std::string::npos) {
        // Seen on the real dock: the DTR reset cuts a status frame mid-line
        // and the Nano's EVT:BOOT gets glued onto that stale fragment.
        RCLCPP_DEBUG(get_logger(), "event after a partial line: '%s'", line.c_str());
        handle_event(line.substr(evt));
      } else {
        RCLCPP_DEBUG(get_logger(), "discarding malformed line: '%s'", line.c_str());
      }
    }

    if (!lines.empty()) {
      last_rx_time_ = now();
    } else if (now() - last_rx_time_ > 2s) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "no data from Nano for >2 s (port still open)");
    }
  }

  void handle_event(const std::string & line)
  {
    RCLCPP_INFO(get_logger(), "%s", line.c_str());
    std_msgs::msg::String msg;
    msg.data = line;
    pub_event_->publish(msg);

    // EVT:BOOT:<ver> / EVT:VER:<ver> (60 s heartbeat) carry the firmware version.
    for (const char * prefix : {"EVT:BOOT:", "EVT:VER:"}) {
      const std::string p{prefix};
      if (line.compare(0, p.size(), p) == 0 && line.size() > p.size()) {
        const std::string version = line.substr(p.size());
        if (version != firmware_version_) {
          firmware_version_ = version;
          std_msgs::msg::String v;
          v.data = version;
          pub_firmware_version_->publish(v);
        }
      }
    }

    // The Nano is out of its bootloader once EVT:BOOT arrives: push our
    // parameter set over the RAM defaults it just printed (EVT:CFG follows
    // for each, so the journal shows defaults first, then ours).
    if (line.rfind("EVT:BOOT", 0) == 0) {
      sync_nano_config("EVT:BOOT");
      return;
    }

    if (const auto cfg = parse_config_event(line)) {
      handle_config_event(*cfg);
      return;
    }

    if (line == "EVT:SELFTEST:OK" || line == "EVT:SELFTEST:FAIL") {
      std_msgs::msg::Bool result;
      result.data = line == "EVT:SELFTEST:OK";
      pub_self_test_result_->publish(result);
    }
  }

  void handle_status(const StatusFrame & frame)
  {
    if (!last_frame_ || last_frame_->state != frame.state) {
      RCLCPP_INFO(
        get_logger(), "state: %s -> %s",
        last_frame_ ? state_name(last_frame_->state) : "(none)",
        state_name(frame.state));
    }
    if ((!last_frame_ || last_frame_->fault_code != frame.fault_code) &&
      frame.fault_code != 0)
    {
      RCLCPP_WARN(
        get_logger(), "fault %d (%s)", frame.fault_code, fault_name(frame.fault_code));
    }
    const bool rebooted = last_frame_ && frame.uptime_s < last_frame_->uptime_s;
    if (rebooted) {
      RCLCPP_INFO(get_logger(), "Nano uptime reset: rebooted (DTR reset or power cycle)");
    }
    // Fallback for a lost/garbled EVT:BOOT line: a status frame means the
    // firmware is running, so it is safe to (re)send the parameter set —
    // but not in the first seconds after open(): frames still in flight
    // from before the DTR reset arrive then, while the Nano is actually in
    // its bootloader and would lose (or misread) anything we send.
    // frames_since_sync_ == 0 right after a BOOT sync is the normal case.
    if (!nano_ready_ && now() - port_opened_ > kBootGrace) {
      sync_nano_config("status frames but no EVT:BOOT seen");
    } else if (nano_ready_ && rebooted && frames_since_sync_ > 0) {
      sync_nano_config("uptime reset without EVT:BOOT");
    }
    ++frames_since_sync_;

    publish_int(*pub_state_, frame.state);
    publish_bool(*pub_microswitch_, frame.microswitch);
    publish_bool(*pub_relay_k1_, frame.k1);
    publish_bool(*pub_relay_k2_, frame.k2);
    publish_int(*pub_fault_, frame.fault_code);
    publish_float(*pub_charge_current_, frame.charge_current);
    publish_float(*pub_charger_voltage_, frame.charger_voltage);
    pub_battery_state_->publish(make_battery_state(frame));

    last_frame_ = frame;
  }

  // The divider only sees the pack when a relay path connects it; near zero
  // volts means "dock cold", not an empty battery.
  BatteryState make_battery_state(const StatusFrame & frame)
  {
    BatteryState bat;
    bat.header.stamp = now();
    bat.header.frame_id = "dock";

    const bool voltage_valid = frame.charger_voltage > 20.0f;
    bat.voltage = voltage_valid ? frame.charger_voltage : NAN;
    bat.current = frame.charge_current;  // positive = charging, per BatteryState
    bat.temperature = NAN;
    bat.charge = NAN;
    bat.capacity = NAN;
    bat.design_capacity = NAN;
    bat.percentage = voltage_valid ?
      std::clamp(
      (frame.charger_voltage - static_cast<float>(v_empty_)) /
      static_cast<float>(v_full_ - v_empty_), 0.0f, 1.0f) :
      NAN;

    switch (frame.state) {
      case 2:  // RAMP
      case 3:  // CHARGING
      case 4:  // DRAIN: caps still discharging into the pack
        bat.power_supply_status = BatteryState::POWER_SUPPLY_STATUS_CHARGING;
        break;
      case 5:  // COMPLETE
        bat.power_supply_status = BatteryState::POWER_SUPPLY_STATUS_FULL;
        break;
      case 1:  // SEATED
      case 6:  // SELFTEST
      case 7:  // FAULT
        bat.power_supply_status = BatteryState::POWER_SUPPLY_STATUS_NOT_CHARGING;
        break;
      default:  // IDLE: no robot, nothing to know
        bat.power_supply_status = BatteryState::POWER_SUPPLY_STATUS_UNKNOWN;
        break;
    }

    switch (frame.fault_code) {
      case 0:
        bat.power_supply_health = BatteryState::POWER_SUPPLY_HEALTH_GOOD;
        break;
      case 4:
        bat.power_supply_health = BatteryState::POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
        break;
      default:
        bat.power_supply_health = BatteryState::POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
        break;
    }

    bat.power_supply_technology = BatteryState::POWER_SUPPLY_TECHNOLOGY_LION;
    bat.present = frame.microswitch;
    return bat;
  }

  void publish_int(rclcpp::Publisher<std_msgs::msg::Int32> & pub, int value)
  {
    std_msgs::msg::Int32 msg;
    msg.data = value;
    pub.publish(msg);
  }
  void publish_bool(rclcpp::Publisher<std_msgs::msg::Bool> & pub, bool value)
  {
    std_msgs::msg::Bool msg;
    msg.data = value;
    pub.publish(msg);
  }
  void publish_float(rclcpp::Publisher<std_msgs::msg::Float32> & pub, float value)
  {
    std_msgs::msg::Float32 msg;
    msg.data = value;
    pub.publish(msg);
  }

  std::string device_;
  int baud_{115200};
  rclcpp::Duration reconnect_interval_{0, 0};
  int pulse_frames_{3};
  bool charge_enable_{true};
  double v_empty_{33.0};
  double v_full_{42.0};

  SerialPort port_;
  std::optional<StatusFrame> last_frame_;
  std::string firmware_version_;
  bool nano_ready_{false};        // firmware running (EVT:BOOT seen) and parameters synced
  uint64_t frames_since_sync_{0};
  rclcpp::Time port_opened_{0, 0, RCL_ROS_TIME};
  // DTR reset + bootloader wait on the Nano is ~1.5 s; stale frames arrive
  // well inside that.
  static constexpr std::chrono::seconds kBootGrace{3};
  int clear_fault_pulse_{0};
  int self_test_pulse_{0};
  rclcpp::Time last_connect_attempt_{0, 0, RCL_ROS_TIME};
  rclcpp::Time last_rx_time_{0, 0, RCL_ROS_TIME};

  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr pub_state_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_microswitch_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_relay_k1_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_relay_k2_;
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr pub_fault_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_self_test_result_;
  rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr pub_charge_current_;
  rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr pub_charger_voltage_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_event_;
  rclcpp::Publisher<BatteryState>::SharedPtr pub_battery_state_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_charge_enable_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_firmware_version_;
  std::map<std::string, rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr> pub_config_float_;
  std::map<std::string, rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr> pub_config_int_;
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr on_set_cb_;
  rclcpp::node_interfaces::PostSetParametersCallbackHandle::SharedPtr post_set_cb_;

  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr sub_charge_enable_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr sub_clear_fault_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr sub_self_test_;

  rclcpp::TimerBase::SharedPtr tx_timer_;
  rclcpp::TimerBase::SharedPtr rx_timer_;
};

}  // namespace mowbot_dock

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<mowbot_dock::DockAgentNode>());
  rclcpp::shutdown();
  return 0;
}
