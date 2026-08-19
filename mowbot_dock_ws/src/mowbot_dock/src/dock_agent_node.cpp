// dock_agent_node: bridges the dock Arduino Nano (USB serial) to ROS 2.
//
// RX: 10 Hz status frames -> dock/* topics + dock/battery_state.
// TX: 10 Hz command frames <chargeEnable>,<clearFault>,<ledMode>,<selfTest>
//     (also the Nano's watchdog feed; the Nano tolerates 2000 ms silence).
//
// clearFault and selfTest are edge-triggered on the Nano and must return to
// 0 after the pulse, so a `true` on the corresponding command topic is
// stretched over a few frames and then auto-cleared.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

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

    sub_charge_enable_ = create_subscription<std_msgs::msg::Bool>(
      "dock/charge_enable_cmd", stream,
      [this](const std_msgs::msg::Bool & msg) {
        if (charge_enable_ != msg.data) {
          RCLCPP_INFO(get_logger(), "charge_enable -> %s", msg.data ? "true" : "false");
        }
        charge_enable_ = msg.data;
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
    try_open();
  }

private:
  void try_open()
  {
    last_connect_attempt_ = now();
    if (port_.open(device_, baud_)) {
      RCLCPP_INFO(
        get_logger(), "opened %s @ %d baud (DTR reset: Nano rebooting)",
        device_.c_str(), baud_);
      last_rx_time_ = now();
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
  }

  void tx_tick()
  {
    if (!port_.is_open()) {
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
    if (last_frame_ && frame.uptime_s < last_frame_->uptime_s) {
      RCLCPP_INFO(get_logger(), "Nano uptime reset: rebooted (DTR reset or power cycle)");
    }

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
